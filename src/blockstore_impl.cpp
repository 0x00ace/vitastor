// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include "blockstore_impl.h"

blockstore_impl_t::blockstore_impl_t(blockstore_config_t & config, ring_loop_t *ringloop)
{
    assert(sizeof(blockstore_op_private_t) <= BS_OP_PRIVATE_DATA_SIZE);
    this->ringloop = ringloop;
    ring_consumer.loop = [this]() { loop(); };
    ringloop->register_consumer(&ring_consumer);
    initialized = 0;
    zero_object = (uint8_t*)memalign_or_die(MEM_ALIGNMENT, block_size);
    data_fd = meta_fd = journal.fd = -1;
    parse_config(config);
    try
    {
        open_data();
        open_meta();
        open_journal();
        calc_lengths();
        data_alloc = new allocator(block_count);
    }
    catch (std::exception & e)
    {
        if (data_fd >= 0)
            close(data_fd);
        if (meta_fd >= 0 && meta_fd != data_fd)
            close(meta_fd);
        if (journal.fd >= 0 && journal.fd != meta_fd)
            close(journal.fd);
        throw;
    }
    flusher = new journal_flusher_t(flusher_count, this);
}

blockstore_impl_t::~blockstore_impl_t()
{
    delete data_alloc;
    delete flusher;
    free(zero_object);
    ringloop->unregister_consumer(&ring_consumer);
    if (data_fd >= 0)
        close(data_fd);
    if (meta_fd >= 0 && meta_fd != data_fd)
        close(meta_fd);
    if (journal.fd >= 0 && journal.fd != meta_fd)
        close(journal.fd);
    if (metadata_buffer)
        free(metadata_buffer);
    if (clean_bitmap)
        free(clean_bitmap);
}

bool blockstore_impl_t::is_started()
{
    return initialized == 10;
}

bool blockstore_impl_t::is_stalled()
{
    return queue_stall;
}

// main event loop - produce requests
void blockstore_impl_t::loop()
{
    // FIXME: initialized == 10 is ugly
    if (initialized != 10)
    {
        // read metadata, then journal
        if (initialized == 0)
        {
            metadata_init_reader = new blockstore_init_meta(this);
            initialized = 1;
        }
        if (initialized == 1)
        {
            int res = metadata_init_reader->loop();
            if (!res)
            {
                delete metadata_init_reader;
                metadata_init_reader = NULL;
                journal_init_reader = new blockstore_init_journal(this);
                initialized = 2;
            }
        }
        if (initialized == 2)
        {
            int res = journal_init_reader->loop();
            if (!res)
            {
                delete journal_init_reader;
                journal_init_reader = NULL;
                initialized = 10;
                ringloop->wakeup();
            }
        }
    }
    else
    {
        // try to submit ops
        unsigned initial_ring_space = ringloop->space_left();
        auto cur = submit_queue.begin();
        // has_writes == 0 - no writes before the current queue item
        // has_writes == 1 - some writes in progress
        // has_writes == 2 - tried to submit some writes, but failed
        int has_writes = 0;
        while (cur != submit_queue.end())
        {
            auto op_ptr = cur;
            auto op = *(cur++);
            // FIXME: This needs some simplification
            // Writes should not block reads if the ring is not full and reads don't depend on them
            // In all other cases we should stop submission
            if (PRIV(op)->wait_for)
            {
                check_wait(op);
                if (PRIV(op)->wait_for == WAIT_SQE)
                {
                    break;
                }
                else if (PRIV(op)->wait_for)
                {
                    if (op->opcode == BS_OP_WRITE || op->opcode == BS_OP_WRITE_STABLE || op->opcode == BS_OP_DELETE)
                    {
                        has_writes = 2;
                    }
                    continue;
                }
            }
            unsigned ring_space = ringloop->space_left();
            unsigned prev_sqe_pos = ringloop->save();
            bool dequeue_op = false, cancel_op = false;
            bool has_in_progress_sync = false;
            if (op->opcode == BS_OP_READ)
            {
                dequeue_op = dequeue_read(op);
                cancel_op = !dequeue_op;
            }
            else if (op->opcode == BS_OP_WRITE || op->opcode == BS_OP_WRITE_STABLE)
            {
                if (has_writes == 2)
                {
                    // Some writes already could not be submitted
                    continue;
                }
                int wr_st = dequeue_write(op);
                // 0 = can't submit
                // 1 = in progress
                // 2 = completed, remove from queue
                dequeue_op = wr_st == 2;
                cancel_op = wr_st == 0;
                has_writes = wr_st > 0 ? 1 : 2;
            }
            else if (op->opcode == BS_OP_DELETE)
            {
                if (has_writes == 2)
                {
                    // Some writes already could not be submitted
                    continue;
                }
                int wr_st = dequeue_del(op);
                dequeue_op = wr_st == 2;
                cancel_op = wr_st == 0;
                has_writes = wr_st > 0 ? 1 : 2;
            }
            else if (op->opcode == BS_OP_SYNC)
            {
                // wait for all small writes to be submitted
                // wait for all big writes to complete, submit data device fsync
                // wait for the data device fsync to complete, then submit journal writes for big writes
                // then submit an fsync operation
                if (has_writes)
                {
                    // Can't submit SYNC before previous writes
                    continue;
                }
                int wr_st = continue_sync(op, has_in_progress_sync);
                dequeue_op = wr_st == 2;
                cancel_op = wr_st == 0;
                if (dequeue_op != 2)
                {
                    // Or we could just set has_writes=1...
                    has_in_progress_sync = true;
                }
            }
            else if (op->opcode == BS_OP_STABLE)
            {
                int wr_st = dequeue_stable(op);
                dequeue_op = wr_st == 2;
                cancel_op = wr_st == 0;
            }
            else if (op->opcode == BS_OP_ROLLBACK)
            {
                int wr_st = dequeue_rollback(op);
                dequeue_op = wr_st == 2;
                cancel_op = wr_st == 0;
            }
            else if (op->opcode == BS_OP_LIST)
            {
                // LIST doesn't need to be blocked by previous modifications
                process_list(op);
                dequeue_op = true;
                cancel_op = false;
            }
            if (dequeue_op)
            {
                submit_queue.erase(op_ptr);
            }
            if (cancel_op)
            {
                ringloop->restore(prev_sqe_pos);
                if (PRIV(op)->wait_for == WAIT_SQE)
                {
                    PRIV(op)->wait_detail = 1 + ring_space;
                    // ring is full, stop submission
                    break;
                }
            }
        }
        if (!readonly)
        {
            flusher->loop();
        }
        int ret = ringloop->submit();
        if (ret < 0)
        {
            throw std::runtime_error(std::string("io_uring_submit: ") + strerror(-ret));
        }
        if ((initial_ring_space - ringloop->space_left()) > 0)
        {
            live = true;
        }
        queue_stall = !live && !ringloop->has_work();
        live = false;
    }
}

bool blockstore_impl_t::is_safe_to_stop()
{
    // It's safe to stop blockstore when there are no in-flight operations,
    // no in-progress syncs and flusher isn't doing anything
    if (submit_queue.size() > 0 || !readonly && flusher->is_active())
    {
        return false;
    }
    if (unsynced_big_writes.size() > 0 || unsynced_small_writes.size() > 0)
    {
        if (!readonly && !stop_sync_submitted)
        {
            // We should sync the blockstore before unmounting
            blockstore_op_t *op = new blockstore_op_t;
            op->opcode = BS_OP_SYNC;
            op->buf = NULL;
            op->callback = [](blockstore_op_t *op)
            {
                delete op;
            };
            enqueue_op(op);
            stop_sync_submitted = true;
        }
        return false;
    }
    return true;
}

void blockstore_impl_t::check_wait(blockstore_op_t *op)
{
    if (PRIV(op)->wait_for == WAIT_SQE)
    {
        if (ringloop->space_left() < PRIV(op)->wait_detail)
        {
            // stop submission if there's still no free space
#ifdef BLOCKSTORE_DEBUG
            printf("Still waiting for %lu SQE(s)\n", PRIV(op)->wait_detail);
#endif
            return;
        }
        PRIV(op)->wait_for = 0;
    }
    else if (PRIV(op)->wait_for == WAIT_JOURNAL)
    {
        if (journal.used_start == PRIV(op)->wait_detail)
        {
            // do not submit
#ifdef BLOCKSTORE_DEBUG
            printf("Still waiting to flush journal offset %08lx\n", PRIV(op)->wait_detail);
#endif
            return;
        }
        flusher->release_trim();
        PRIV(op)->wait_for = 0;
    }
    else if (PRIV(op)->wait_for == WAIT_JOURNAL_BUFFER)
    {
        int next = ((journal.cur_sector + 1) % journal.sector_count);
        if (journal.sector_info[next].flush_count > 0 ||
            journal.sector_info[next].dirty)
        {
            // do not submit
#ifdef BLOCKSTORE_DEBUG
            printf("Still waiting for a journal buffer\n");
#endif
            return;
        }
        PRIV(op)->wait_for = 0;
    }
    else if (PRIV(op)->wait_for == WAIT_FREE)
    {
        if (!data_alloc->get_free_count() && !flusher->is_active())
        {
#ifdef BLOCKSTORE_DEBUG
            printf("Still waiting for free space on the data device\n");
#endif
            return;
        }
        PRIV(op)->wait_for = 0;
    }
    else
    {
        throw std::runtime_error("BUG: op->wait_for value is unexpected");
    }
}

void blockstore_impl_t::enqueue_op(blockstore_op_t *op)
{
    if (op->opcode < BS_OP_MIN || op->opcode > BS_OP_MAX ||
        ((op->opcode == BS_OP_READ || op->opcode == BS_OP_WRITE || op->opcode == BS_OP_WRITE_STABLE) && (
            op->offset >= block_size ||
            op->len > block_size-op->offset ||
            (op->len % disk_alignment)
        )) ||
        readonly && op->opcode != BS_OP_READ && op->opcode != BS_OP_LIST)
    {
        // Basic verification not passed
        op->retval = -EINVAL;
        std::function<void (blockstore_op_t*)>(op->callback)(op);
        return;
    }
    if (op->opcode == BS_OP_SYNC_STAB_ALL)
    {
        std::function<void(blockstore_op_t*)> *old_callback = new std::function<void(blockstore_op_t*)>(op->callback);
        op->opcode = BS_OP_SYNC;
        op->callback = [this, old_callback](blockstore_op_t *op)
        {
            if (op->retval >= 0 && unstable_writes.size() > 0)
            {
                op->opcode = BS_OP_STABLE;
                op->len = unstable_writes.size();
                obj_ver_id *vers = new obj_ver_id[op->len];
                op->buf = vers;
                int i = 0;
                for (auto it = unstable_writes.begin(); it != unstable_writes.end(); it++, i++)
                {
                    vers[i] = {
                        .oid = it->first,
                        .version = it->second,
                    };
                }
                unstable_writes.clear();
                op->callback = [this, old_callback](blockstore_op_t *op)
                {
                    obj_ver_id *vers = (obj_ver_id*)op->buf;
                    delete[] vers;
                    op->buf = NULL;
                    (*old_callback)(op);
                    delete old_callback;
                };
                this->enqueue_op(op);
            }
            else
            {
                (*old_callback)(op);
                delete old_callback;
            }
        };
    }
    if ((op->opcode == BS_OP_WRITE || op->opcode == BS_OP_WRITE_STABLE || op->opcode == BS_OP_DELETE) && !enqueue_write(op))
    {
        std::function<void (blockstore_op_t*)>(op->callback)(op);
        return;
    }
    // Call constructor without allocating memory. We'll call destructor before returning op back
    new ((void*)op->private_data) blockstore_op_private_t;
    PRIV(op)->wait_for = 0;
    PRIV(op)->op_state = 0;
    PRIV(op)->pending_ops = 0;
    submit_queue.push_back(op);
    ringloop->wakeup();
}

static bool replace_stable(object_id oid, uint64_t version, int search_start, int search_end, obj_ver_id* list)
{
    while (search_start < search_end)
    {
        int pos = search_start+(search_end-search_start)/2;
        if (oid < list[pos].oid)
        {
            search_end = pos;
        }
        else if (list[pos].oid < oid)
        {
            search_start = pos+1;
        }
        else
        {
            list[pos].version = version;
            return true;
        }
    }
    return false;
}

void blockstore_impl_t::process_list(blockstore_op_t *op)
{
    uint32_t list_pg = op->offset;
    uint32_t pg_count = op->len;
    uint64_t pg_stripe_size = op->oid.stripe;
    uint64_t min_inode = op->oid.inode;
    uint64_t max_inode = op->version;
    // Check PG
    if (pg_count != 0 && (pg_stripe_size < MIN_BLOCK_SIZE || list_pg >= pg_count))
    {
        op->retval = -EINVAL;
        FINISH_OP(op);
        return;
    }
    // Copy clean_db entries (sorted)
    int stable_count = 0, stable_alloc = clean_db.size() / (pg_count ? pg_count : 1);
    obj_ver_id *stable = (obj_ver_id*)malloc(sizeof(obj_ver_id) * stable_alloc);
    if (!stable)
    {
        op->retval = -ENOMEM;
        FINISH_OP(op);
        return;
    }
    {
        auto clean_it = clean_db.begin(), clean_end = clean_db.end();
        if ((min_inode != 0 || max_inode != 0) && min_inode <= max_inode)
        {
            clean_it = clean_db.lower_bound({
                .inode = min_inode,
                .stripe = 0,
            });
            clean_end = clean_db.upper_bound({
                .inode = max_inode,
                .stripe = UINT64_MAX,
            });
        }
        for (; clean_it != clean_end; clean_it++)
        {
            if (!pg_count || ((clean_it->first.inode + clean_it->first.stripe / pg_stripe_size) % pg_count) == list_pg)
            {
                if (stable_count >= stable_alloc)
                {
                    stable_alloc += 32768;
                    stable = (obj_ver_id*)realloc(stable, sizeof(obj_ver_id) * stable_alloc);
                    if (!stable)
                    {
                        op->retval = -ENOMEM;
                        FINISH_OP(op);
                        return;
                    }
                }
                stable[stable_count++] = {
                    .oid = clean_it->first,
                    .version = clean_it->second.version,
                };
            }
        }
    }
    int clean_stable_count = stable_count;
    // Copy dirty_db entries (sorted, too)
    int unstable_count = 0, unstable_alloc = 0;
    obj_ver_id *unstable = NULL;
    {
        auto dirty_it = dirty_db.begin(), dirty_end = dirty_db.end();
        if ((min_inode != 0 || max_inode != 0) && min_inode <= max_inode)
        {
            dirty_it = dirty_db.lower_bound({
                .oid = {
                    .inode = min_inode,
                    .stripe = 0,
                },
                .version = 0,
            });
            dirty_end = dirty_db.upper_bound({
                .oid = {
                    .inode = max_inode,
                    .stripe = UINT64_MAX,
                },
                .version = UINT64_MAX,
            });
        }
        for (; dirty_it != dirty_end; dirty_it++)
        {
            if (!pg_count || ((dirty_it->first.oid.inode + dirty_it->first.oid.stripe / pg_stripe_size) % pg_count) == list_pg)
            {
                if (IS_DELETE(dirty_it->second.state))
                {
                    // Deletions are always stable, so try to zero out two possible entries
                    if (!replace_stable(dirty_it->first.oid, 0, 0, clean_stable_count, stable))
                    {
                        replace_stable(dirty_it->first.oid, 0, clean_stable_count, stable_count, stable);
                    }
                }
                else if (IS_STABLE(dirty_it->second.state))
                {
                    // First try to replace a clean stable version in the first part of the list
                    if (!replace_stable(dirty_it->first.oid, dirty_it->first.version, 0, clean_stable_count, stable))
                    {
                        // Then try to replace the last dirty stable version in the second part of the list
                        if (stable_count > 0 && stable[stable_count-1].oid == dirty_it->first.oid)
                        {
                            stable[stable_count-1].version = dirty_it->first.version;
                        }
                        else
                        {
                            if (stable_count >= stable_alloc)
                            {
                                stable_alloc += 32768;
                                stable = (obj_ver_id*)realloc(stable, sizeof(obj_ver_id) * stable_alloc);
                                if (!stable)
                                {
                                    if (unstable)
                                        free(unstable);
                                    op->retval = -ENOMEM;
                                    FINISH_OP(op);
                                    return;
                                }
                            }
                            stable[stable_count++] = dirty_it->first;
                        }
                    }
                }
                else
                {
                    if (unstable_count >= unstable_alloc)
                    {
                        unstable_alloc += 32768;
                        unstable = (obj_ver_id*)realloc(unstable, sizeof(obj_ver_id) * unstable_alloc);
                        if (!unstable)
                        {
                            if (stable)
                                free(stable);
                            op->retval = -ENOMEM;
                            FINISH_OP(op);
                            return;
                        }
                    }
                    unstable[unstable_count++] = dirty_it->first;
                }
            }
        }
    }
    // Remove zeroed out stable entries
    int j = 0;
    for (int i = 0; i < stable_count; i++)
    {
        if (stable[i].version != 0)
        {
            stable[j++] = stable[i];
        }
    }
    stable_count = j;
    if (stable_count+unstable_count > stable_alloc)
    {
        stable_alloc = stable_count+unstable_count;
        stable = (obj_ver_id*)realloc(stable, sizeof(obj_ver_id) * stable_alloc);
        if (!stable)
        {
            if (unstable)
                free(unstable);
            op->retval = -ENOMEM;
            FINISH_OP(op);
            return;
        }
    }
    // Copy unstable entries
    for (int i = 0; i < unstable_count; i++)
    {
        stable[j++] = unstable[i];
    }
    free(unstable);
    op->version = stable_count;
    op->retval = stable_count+unstable_count;
    op->buf = stable;
    FINISH_OP(op);
}

// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include "disk_tool.h"
#include "rw_blocking.h"
#include "osd_id.h"

int disk_tool_t::process_meta(std::function<void(blockstore_meta_header_v1_t *)> hdr_fn,
    std::function<void(uint64_t, clean_disk_entry*, uint8_t*)> record_fn)
{
    if (dsk.meta_block_size % DIRECT_IO_ALIGNMENT)
    {
        fprintf(stderr, "Invalid metadata block size: is not a multiple of %d\n", DIRECT_IO_ALIGNMENT);
        return 1;
    }
    dsk.meta_fd = open(dsk.meta_device.c_str(), O_DIRECT|O_RDONLY);
    if (dsk.meta_fd < 0)
    {
        fprintf(stderr, "Failed to open metadata device %s: %s\n", dsk.meta_device.c_str(), strerror(errno));
        return 1;
    }
    int buf_size = 1024*1024;
    if (buf_size % dsk.meta_block_size)
        buf_size = 8*dsk.meta_block_size;
    if (buf_size > dsk.meta_len)
        buf_size = dsk.meta_len;
    void *data = memalign_or_die(MEM_ALIGNMENT, buf_size);
    lseek64(dsk.meta_fd, dsk.meta_offset, 0);
    read_blocking(dsk.meta_fd, data, buf_size);
    // Check superblock
    blockstore_meta_header_v1_t *hdr = (blockstore_meta_header_v1_t *)data;
    if (hdr->zero == 0 &&
        hdr->magic == BLOCKSTORE_META_MAGIC_V1 &&
        hdr->version == BLOCKSTORE_META_VERSION_V1)
    {
        // Vitastor 0.6-0.7 - static array of clean_disk_entry with bitmaps
        if (hdr->meta_block_size != dsk.meta_block_size)
        {
            fprintf(stderr, "Using block size of %u bytes based on information from the superblock\n", hdr->meta_block_size);
            dsk.meta_block_size = hdr->meta_block_size;
            if (buf_size % dsk.meta_block_size)
            {
                buf_size = 8*dsk.meta_block_size;
                free(data);
                data = memalign_or_die(MEM_ALIGNMENT, buf_size);
            }
        }
        dsk.bitmap_granularity = hdr->bitmap_granularity;
        dsk.clean_entry_bitmap_size = hdr->data_block_size / hdr->bitmap_granularity / 8;
        dsk.clean_entry_size = sizeof(clean_disk_entry) + 2*dsk.clean_entry_bitmap_size;
        uint64_t block_num = 0;
        hdr_fn(hdr);
        meta_pos = dsk.meta_block_size;
        lseek64(dsk.meta_fd, dsk.meta_offset+meta_pos, 0);
        while (meta_pos < dsk.meta_len)
        {
            uint64_t read_len = buf_size < dsk.meta_len-meta_pos ? buf_size : dsk.meta_len-meta_pos;
            read_blocking(dsk.meta_fd, data, read_len);
            meta_pos += read_len;
            for (uint64_t blk = 0; blk < read_len; blk += dsk.meta_block_size)
            {
                for (uint64_t ioff = 0; ioff <= dsk.meta_block_size-dsk.clean_entry_size; ioff += dsk.clean_entry_size, block_num++)
                {
                    clean_disk_entry *entry = (clean_disk_entry*)((uint8_t*)data + blk + ioff);
                    if (entry->oid.inode)
                    {
                        record_fn(block_num, entry, entry->bitmap);
                    }
                }
            }
        }
    }
    else
    {
        // Vitastor 0.4-0.5 - static array of clean_disk_entry
        dsk.clean_entry_bitmap_size = 0;
        dsk.clean_entry_size = sizeof(clean_disk_entry);
        uint64_t block_num = 0;
        hdr_fn(NULL);
        while (meta_pos < dsk.meta_len)
        {
            uint64_t read_len = buf_size < dsk.meta_len-meta_pos ? buf_size : dsk.meta_len-meta_pos;
            read_blocking(dsk.meta_fd, data, read_len);
            meta_pos += read_len;
            for (uint64_t blk = 0; blk < read_len; blk += dsk.meta_block_size)
            {
                for (uint64_t ioff = 0; ioff < dsk.meta_block_size-dsk.clean_entry_size; ioff += dsk.clean_entry_size, block_num++)
                {
                    clean_disk_entry *entry = (clean_disk_entry*)((uint8_t*)data + blk + ioff);
                    if (entry->oid.inode)
                    {
                        record_fn(block_num, entry, NULL);
                    }
                }
            }
        }
    }
    free(data);
    close(dsk.meta_fd);
    dsk.meta_fd = -1;
    return 0;
}

int disk_tool_t::dump_meta()
{
    int r = process_meta(
        [this](blockstore_meta_header_v1_t *hdr) { dump_meta_header(hdr); },
        [this](uint64_t block_num, clean_disk_entry *entry, uint8_t *bitmap) { dump_meta_entry(block_num, entry, bitmap); }
    );
    printf("\n]}\n");
    return r;
}

void disk_tool_t::dump_meta_header(blockstore_meta_header_v1_t *hdr)
{
    if (hdr)
    {
        printf(
            "{\"version\":\"0.6\",\"meta_block_size\":%u,\"data_block_size\":%u,\"bitmap_granularity\":%u,\"entries\":[\n",
            hdr->meta_block_size, hdr->data_block_size, hdr->bitmap_granularity
        );
    }
    else
    {
        printf("{\"version\":\"0.5\",\"meta_block_size\":%lu,\"entries\":[\n", dsk.meta_block_size);
    }
    first = true;
}

void disk_tool_t::dump_meta_entry(uint64_t block_num, clean_disk_entry *entry, uint8_t *bitmap)
{
    printf(
#define ENTRY_FMT "{\"block\":%lu,\"pool\":%u,\"inode\":%lu,\"stripe\":%lu,\"version\":%lu"
        (first ? ENTRY_FMT : (",\n" ENTRY_FMT)),
#undef ENTRY_FMT
        block_num, INODE_POOL(entry->oid.inode), INODE_NO_POOL(entry->oid.inode),
        entry->oid.stripe, entry->version
    );
    if (bitmap)
    {
        printf(",\"bitmap\":\"");
        for (uint64_t i = 0; i < dsk.clean_entry_bitmap_size; i++)
        {
            printf("%02x", bitmap[i]);
        }
        printf("\",\"ext_bitmap\":\"");
        for (uint64_t i = 0; i < dsk.clean_entry_bitmap_size; i++)
        {
            printf("%02x", bitmap[dsk.clean_entry_bitmap_size + i]);
        }
        printf("\"}");
    }
    else
    {
        printf("}");
    }
    first = false;
}
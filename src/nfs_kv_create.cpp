// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)
//
// NFS proxy over VitastorKV database - CREATE, MKDIR, SYMLINK, MKNOD

#include <sys/time.h>

#include "str_util.h"
#include "nfs_proxy.h"
#include "nfs_kv.h"

void allocate_new_id(nfs_client_t *self, std::function<void(int res, uint64_t new_id)> cb)
{
    if (self->parent->kvfs->fs_next_id <= self->parent->kvfs->fs_allocated_id)
    {
        cb(0, self->parent->kvfs->fs_next_id++);
        return;
    }
    else if (self->parent->kvfs->fs_next_id > self->parent->fs_inode_count)
    {
        cb(-ENOSPC, 0);
        return;
    }
    self->parent->db->get(KV_NEXT_ID_KEY, [=](int res, const std::string & prev_str)
    {
        if (res < 0 && res != -ENOENT)
        {
            cb(res, 0);
            return;
        }
        uint64_t prev_val = stoull_full(prev_str);
        if (prev_val >= self->parent->fs_inode_count)
        {
            cb(-ENOSPC, 0);
            return;
        }
        if (prev_val < 1)
        {
            prev_val = 1;
        }
        uint64_t new_val = prev_val + self->parent->id_alloc_batch_size;
        if (new_val >= self->parent->fs_inode_count)
        {
            new_val = self->parent->fs_inode_count;
        }
        self->parent->db->set(KV_NEXT_ID_KEY, std::to_string(new_val), [=](int res)
        {
            if (res == -EAGAIN)
            {
                // CAS failure - retry
                allocate_new_id(self, cb);
            }
            else if (res < 0)
            {
                cb(res, 0);
            }
            else
            {
                self->parent->kvfs->fs_next_id = prev_val+2;
                self->parent->kvfs->fs_allocated_id = new_val;
                cb(0, prev_val+1);
            }
        }, [prev_val](int res, const std::string & value)
        {
            // FIXME: Allow to modify value from CAS callback? ("update" query)
            return res < 0 || stoull_full(value) == prev_val;
        });
    });
}

struct kv_create_state
{
    nfs_client_t *self = NULL;
    rpc_op_t *rop = NULL;
    bool exclusive = false;
    uint64_t verf = 0;
    uint64_t dir_ino = 0;
    std::string filename;
    uint64_t new_id = 0;
    json11::Json::object attrobj;
    json11::Json attrs;
    std::string direntry_text;
    uint64_t dup_ino = 0;
    std::function<void(int res)> cb;
};

static void kv_do_create(kv_create_state *st)
{
    if (st->self->parent->trace)
        fprintf(stderr, "[%d] CREATE %ju/%s ATTRS %s\n", st->self->nfs_fd, st->dir_ino, st->filename.c_str(), json11::Json(st->attrobj).dump().c_str());
    if (st->filename == "" || st->filename.find("/") != std::string::npos)
    {
        auto cb = std::move(st->cb);
        cb(-EINVAL);
        return;
    }
    // Generate inode ID
    allocate_new_id(st->self, [st](int res, uint64_t new_id)
    {
        if (res < 0)
        {
            auto cb = std::move(st->cb);
            cb(res);
            return;
        }
        st->new_id = new_id;
        auto direntry = json11::Json::object{ { "ino", st->new_id } };
        if (st->attrobj["type"].string_value() == "dir")
        {
            direntry["type"] = "dir";
        }
        st->attrs = std::move(st->attrobj);
        st->direntry_text = json11::Json(direntry).dump().c_str();
        // Set direntry
        st->self->parent->db->set(kv_direntry_key(st->dir_ino, st->filename), st->direntry_text, [st](int res)
        {
            if (res < 0)
            {
                st->self->parent->kvfs->unallocated_ids.push_back(st->new_id);
                if (res == -EAGAIN)
                {
                    if (st->dup_ino)
                    {
                        st->new_id = st->dup_ino;
                        res = 0;
                    }
                    else
                        res = -EEXIST;
                }
                else
                    fprintf(stderr, "create %ju/%s failed: %s (code %d)\n", st->dir_ino, st->filename.c_str(), strerror(-res), res);
                auto cb = std::move(st->cb);
                cb(res);
            }
            else
            {
                st->self->parent->db->set(kv_inode_key(st->new_id), st->attrs.dump().c_str(), [st](int res)
                {
                    if (res == -EAGAIN)
                    {
                        res = -EEXIST;
                    }
                    if (res < 0)
                    {
                        st->self->parent->db->del(kv_direntry_key(st->dir_ino, st->filename), [st, res](int del_res)
                        {
                            if (!del_res)
                            {
                                st->self->parent->kvfs->unallocated_ids.push_back(st->new_id);
                            }
                            auto cb = std::move(st->cb);
                            cb(res);
                        }, [st](int res, const std::string & value)
                        {
                            return res != -ENOENT && value == st->direntry_text;
                        });
                    }
                    else
                    {
                        auto cb = std::move(st->cb);
                        cb(0);
                    }
                }, [st](int res, const std::string & value)
                {
                    return res == -ENOENT;
                });
            }
        }, [st](int res, const std::string & value)
        {
            // CAS compare - check that the key doesn't exist
            if (res == 0)
            {
                std::string err;
                auto direntry = json11::Json::parse(value, err);
                if (err != "")
                {
                    fprintf(stderr, "Invalid JSON in direntry %s = %s: %s, overwriting\n",
                        kv_direntry_key(st->dir_ino, st->filename).c_str(), value.c_str(), err.c_str());
                    return true;
                }
                if (st->exclusive && direntry["verf"].uint64_value() == st->verf)
                {
                    st->dup_ino = direntry["ino"].uint64_value();
                    return false;
                }
                return false;
            }
            return true;
        });
    });
}

static void kv_create_setattr(json11::Json::object & attrobj, sattr3 & sattr)
{
    if (sattr.mode.set_it)
        attrobj["mode"] = (uint64_t)sattr.mode.mode;
    if (sattr.uid.set_it)
        attrobj["uid"] = (uint64_t)sattr.uid.uid;
    if (sattr.gid.set_it)
        attrobj["gid"] = (uint64_t)sattr.gid.gid;
    if (sattr.atime.set_it)
        attrobj["atime"] = nfstime_to_str(sattr.atime.atime);
    if (sattr.mtime.set_it)
        attrobj["mtime"] = nfstime_to_str(sattr.mtime.mtime);
}

template<class T, class Tok> static void kv_create_reply(kv_create_state *st, int res)
{
    T *reply = (T*)st->rop->reply;
    if (res < 0)
    {
        *reply = (T){ .status = vitastor_nfs_map_err(-res) };
    }
    else
    {
        *reply = (T){
            .status = NFS3_OK,
            .resok = (Tok){
                .obj = {
                    .handle_follows = 1,
                    .handle = xdr_copy_string(st->rop->xdrs, kv_fh(st->new_id)),
                },
                .obj_attributes = {
                    .attributes_follow = 1,
                    .attributes = get_kv_attributes(st->self, st->new_id, st->attrs),
                },
            },
        };
    }
    rpc_queue_reply(st->rop);
    delete st;
}

int kv_nfs3_create_proc(void *opaque, rpc_op_t *rop)
{
    kv_create_state *st = new kv_create_state;
    st->self = (nfs_client_t*)opaque;
    st->rop = rop;
    auto args = (CREATE3args*)rop->request;
    st->exclusive = args->how.mode == NFS_EXCLUSIVE;
    st->verf = st->exclusive ? *(uint64_t*)&args->how.verf : 0;
    st->dir_ino = kv_fh_inode(args->where.dir);
    st->filename = args->where.name;
    if (args->how.mode == NFS_EXCLUSIVE)
    {
        st->attrobj["verf"] = *(uint64_t*)&args->how.verf;
    }
    else if (args->how.mode == NFS_UNCHECKED)
    {
        kv_create_setattr(st->attrobj, args->how.obj_attributes);
        if (args->how.obj_attributes.size.set_it)
        {
            st->attrobj["size"] = (uint64_t)args->how.obj_attributes.size.size;
            st->attrobj["empty"] = true;
        }
    }
    st->cb = [st](int res) { kv_create_reply<CREATE3res, CREATE3resok>(st, res); };
    kv_do_create(st);
    return 1;
}

int kv_nfs3_mkdir_proc(void *opaque, rpc_op_t *rop)
{
    kv_create_state *st = new kv_create_state;
    st->self = (nfs_client_t*)opaque;
    st->rop = rop;
    auto args = (MKDIR3args*)rop->request;
    st->dir_ino = kv_fh_inode(args->where.dir);
    st->filename = args->where.name;
    st->attrobj["type"] = "dir";
    st->attrobj["parent_ino"] = st->dir_ino;
    kv_create_setattr(st->attrobj, args->attributes);
    st->cb = [st](int res) { kv_create_reply<MKDIR3res, MKDIR3resok>(st, res); };
    kv_do_create(st);
    return 1;
}

int kv_nfs3_symlink_proc(void *opaque, rpc_op_t *rop)
{
    kv_create_state *st = new kv_create_state;
    st->self = (nfs_client_t*)opaque;
    st->rop = rop;
    auto args = (SYMLINK3args*)rop->request;
    st->dir_ino = kv_fh_inode(args->where.dir);
    st->filename = args->where.name;
    st->attrobj["type"] = "link";
    st->attrobj["symlink"] = (std::string)args->symlink.symlink_data;
    kv_create_setattr(st->attrobj, args->symlink.symlink_attributes);
    st->cb = [st](int res) { kv_create_reply<SYMLINK3res, SYMLINK3resok>(st, res); };
    kv_do_create(st);
    return 1;
}

int kv_nfs3_mknod_proc(void *opaque, rpc_op_t *rop)
{
    kv_create_state *st = new kv_create_state;
    st->self = (nfs_client_t*)opaque;
    st->rop = rop;
    auto args = (MKNOD3args*)rop->request;
    st->dir_ino = kv_fh_inode(args->where.dir);
    st->filename = args->where.name;
    if (args->what.type == NF3CHR || args->what.type == NF3BLK)
    {
        st->attrobj["type"] = (args->what.type == NF3CHR ? "chr" : "blk");
        st->attrobj["major"] = (uint64_t)args->what.chr_device.spec.specdata1;
        st->attrobj["minor"] = (uint64_t)args->what.chr_device.spec.specdata2;
        kv_create_setattr(st->attrobj, args->what.chr_device.dev_attributes);
    }
    else if (args->what.type == NF3SOCK || args->what.type == NF3FIFO)
    {
        st->attrobj["type"] = (args->what.type == NF3SOCK ? "sock" : "fifo");
        kv_create_setattr(st->attrobj, args->what.sock_attributes);
    }
    else
    {
        *(MKNOD3res*)rop->reply = (MKNOD3res){ .status = NFS3ERR_INVAL };
        rpc_queue_reply(rop);
        delete st;
        return 0;
    }
    st->cb = [st](int res) { kv_create_reply<MKNOD3res, MKNOD3resok>(st, res); };
    kv_do_create(st);
    return 1;
}
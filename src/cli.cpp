// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

/**
 * CLI tool
 * Currently can (a) remove inodes and (b) merge snapshot/clone layers
 */

#include <vector>
#include <algorithm>

#include "cli.h"
#include "epoll_manager.h"
#include "cluster_client.h"
#include "pg_states.h"
#include "base64.h"

static const char *exe_name = NULL;

json11::Json::object cli_tool_t::parse_args(int narg, const char *args[])
{
    json11::Json::object cfg;
    json11::Json::array cmd;
    cfg["progress"] = "1";
    for (int i = 1; i < narg; i++)
    {
        if (!strcmp(args[i], "-h") || !strcmp(args[i], "--help"))
        {
            help();
        }
        else if (args[i][0] == '-' && args[i][1] == 'l')
        {
            cfg["long"] = "1";
        }
        else if (args[i][0] == '-' && args[i][1] == 'n')
        {
            cfg["count"] = args[++i];
        }
        else if (args[i][0] == '-' && args[i][1] == 'i')
        {
            cfg["interactive"] = "1";
        }
        else if (args[i][0] == '-' && args[i][1] == 'p')
        {
            cfg["pool"] = args[++i];
        }
        else if (args[i][0] == '-' && args[i][1] == 's')
        {
            cfg["size"] = args[++i];
        }
        else if (args[i][0] == '-' && args[i][1] == '-')
        {
            const char *opt = args[i]+2;
            cfg[opt] = i == narg-1 || !strcmp(opt, "json") || !strcmp(opt, "wait-list") ||
                !strcmp(opt, "long") || !strcmp(opt, "writers-stopped") && strcmp("1", args[i+1]) != 0
                ? "1" : args[++i];
        }
        else
        {
            cmd.push_back(std::string(args[i]));
        }
    }
    if (!cmd.size())
    {
        std::string exe(exe_name);
        if (exe.substr(exe.size()-11) == "vitastor-rm")
        {
            cmd.push_back("rm-data");
        }
    }
    cfg["command"] = cmd;
    return cfg;
}

void cli_tool_t::help()
{
    printf(
        "Vitastor command-line tool\n"
        "(c) Vitaliy Filippov, 2019+ (VNPL-1.1)\n"
        "\n"
        "USAGE:\n"
        "%s ls [-l] [--pool|-p <id|name>]\n"
        "  List existing images from a specified pool or from all pools if not specified.\n"
        "  Also report allocated size if -l is specified.\n"
        "\n"
        "%s create -s|--size <size> [--pool <id|name>] [--parent <parent_name>[@<snapshot>]] <name>\n"
        "  Create an image. You may use K/M/G/T suffixes for <size>. If --parent is specified,\n"
        "  a copy-on-write image clone is created. Parent must be a snapshot (readonly image).\n"
        "  Pool must be specified if there is more than one pool.\n"
        "\n"
        "%s create --snapshot <snapshot> [--pool <id|name>] <image>\n"
        "%s snap-create [--pool <id|name>] <image>@<snapshot>\n"
        "  Create a snapshot of image <name>. May be used live if only a single writer is active.\n"
        "\n"
        "%s set <name> [-s|--size <size>] [--readonly | --readwrite]\n"
        "  Resize image or change its readonly status. Images with children can't be made read-write.\n"
        "\n"
        "%s top [-n <MAX_COUNT>] [-i]\n"
        "  Disable image list sorted by I/O load, interactive if -i specified.\n"
        "\n"
        "%s rm [OPTIONS] <from> [<to>] [--writers-stopped]\n"
        "  Remove <from> or all layers between <from> and <to> (<to> must be a child of <from>),\n"
        "  rebasing all their children accordingly. --writers-stopped allows merging to be a bit\n"
        "  more effective in case of a single 'slim' read-write child and 'fat' removed parent:\n"
        "  the child is merged into parent in that case and parent is renamed to child.\n"
        "  In other cases parent layers are always merged into children.\n"
        "\n"
        "%s flatten [OPTIONS] <layer>\n"
        "  Flatten a layer, i.e. merge data and detach it from parents.\n"
        "\n"
        "%s rm-data [OPTIONS] --pool <pool> --inode <inode> [--wait-list]\n"
        "  Remove inode data without changing metadata.\n"
        "  --wait-list means first retrieve objects listings and then remove it.\n"
        "  --wait-list requires more memory, but allows to show correct removal progress.\n"
        "\n"
        "%s merge-data [OPTIONS] <from> <to> [--target <target>]\n"
        "  Merge layer data without changing metadata. Merge <from>..<to> to <target>.\n"
        "  <to> must be a child of <from> and <target> may be one of the layers between\n"
        "  <from> and <to>, including <from> and <to>.\n"
        "\n"
        "OPTIONS (global):\n"
        "  --etcd_address <etcd_address>\n"
        "  --iodepth N         Send N operations in parallel to each OSD when possible (default 32)\n"
        "  --parallel_osds M   Work with M osds in parallel when possible (default 4)\n"
        "  --progress 1|0      Report progress (default 1)\n"
        "  --cas 1|0           Use online CAS writes when possible (default auto)\n"
        "  --json              JSON output\n"
        ,
        exe_name, exe_name, exe_name, exe_name, exe_name, exe_name, exe_name, exe_name, exe_name, exe_name
    );
    exit(0);
}

void cli_tool_t::change_parent(inode_t cur, inode_t new_parent)
{
    auto cur_cfg_it = cli->st_cli.inode_config.find(cur);
    if (cur_cfg_it == cli->st_cli.inode_config.end())
    {
        fprintf(stderr, "Inode 0x%lx disappeared\n", cur);
        exit(1);
    }
    inode_config_t new_cfg = cur_cfg_it->second;
    std::string cur_name = new_cfg.name;
    std::string cur_cfg_key = base64_encode(cli->st_cli.etcd_prefix+
        "/config/inode/"+std::to_string(INODE_POOL(cur))+
        "/"+std::to_string(INODE_NO_POOL(cur)));
    new_cfg.parent_id = new_parent;
    json11::Json::object cur_cfg_json = cli->st_cli.serialize_inode_cfg(&new_cfg);
    waiting++;
    cli->st_cli.etcd_txn(json11::Json::object {
        { "compare", json11::Json::array {
            json11::Json::object {
                { "target", "MOD" },
                { "key", cur_cfg_key },
                { "result", "LESS" },
                { "mod_revision", new_cfg.mod_revision+1 },
            },
        } },
        { "success", json11::Json::array {
            json11::Json::object {
                { "request_put", json11::Json::object {
                    { "key", cur_cfg_key },
                    { "value", base64_encode(json11::Json(cur_cfg_json).dump()) },
                } }
            },
        } },
    }, ETCD_SLOW_TIMEOUT, [this, new_parent, cur, cur_name](std::string err, json11::Json res)
    {
        if (err != "")
        {
            fprintf(stderr, "Error changing parent of %s: %s\n", cur_name.c_str(), err.c_str());
            exit(1);
        }
        if (!res["succeeded"].bool_value())
        {
            fprintf(stderr, "Inode %s was modified during snapshot deletion\n", cur_name.c_str());
            exit(1);
        }
        if (new_parent)
        {
            auto new_parent_it = cli->st_cli.inode_config.find(new_parent);
            std::string new_parent_name = new_parent_it != cli->st_cli.inode_config.end()
                ? new_parent_it->second.name : "<unknown>";
            printf(
                "Parent of layer %s (inode %lu in pool %u) changed to %s (inode %lu in pool %u)\n",
                cur_name.c_str(), INODE_NO_POOL(cur), INODE_POOL(cur),
                new_parent_name.c_str(), INODE_NO_POOL(new_parent), INODE_POOL(new_parent)
            );
        }
        else
        {
            printf(
                "Parent of layer %s (inode %lu in pool %u) detached\n",
                cur_name.c_str(), INODE_NO_POOL(cur), INODE_POOL(cur)
            );
        }
        waiting--;
        ringloop->wakeup();
    });
}

inode_config_t* cli_tool_t::get_inode_cfg(const std::string & name)
{
    for (auto & ic: cli->st_cli.inode_config)
    {
        if (ic.second.name == name)
        {
            return &ic.second;
        }
    }
    fprintf(stderr, "Layer %s not found\n", name.c_str());
    exit(1);
}

void cli_tool_t::run(json11::Json cfg)
{
    json11::Json::array cmd = cfg["command"].array_items();
    if (!cmd.size())
    {
        fprintf(stderr, "command is missing\n");
        exit(1);
    }
    else if (cmd[0] == "ls")
    {
        // List images
        action_cb = start_ls(cfg);
    }
    else if (cmd[0] == "create" || cmd[0] == "snap-create")
    {
        // Create image/snapshot
        action_cb = start_create(cfg);
    }
    else if (cmd[0] == "rm-data")
    {
        // Delete inode data
        action_cb = start_rm(cfg);
    }
    else if (cmd[0] == "merge-data")
    {
        // Merge layer data without affecting metadata
        action_cb = start_merge(cfg);
    }
    else if (cmd[0] == "flatten")
    {
        // Merge layer data without affecting metadata
        action_cb = start_flatten(cfg);
    }
    else if (cmd[0] == "rm")
    {
        // Remove multiple snapshots and rebase their children
        action_cb = start_snap_rm(cfg);
    }
    else
    {
        fprintf(stderr, "unknown command: %s\n", cmd[0].string_value().c_str());
        exit(1);
    }
    json_output = cfg["json"].bool_value();
    iodepth = cfg["iodepth"].uint64_value();
    if (!iodepth)
        iodepth = 32;
    parallel_osds = cfg["parallel_osds"].uint64_value();
    if (!parallel_osds)
        parallel_osds = 4;
    log_level = cfg["log_level"].int64_value();
    progress = cfg["progress"].uint64_value() ? true : false;
    list_first = cfg["wait-list"].uint64_value() ? true : false;
    // Create client
    ringloop = new ring_loop_t(512);
    epmgr = new epoll_manager_t(ringloop);
    cli = new cluster_client_t(ringloop, epmgr->tfd, cfg);
    cli->on_ready([this]()
    {
        // Initialize job
        consumer.loop = [this]()
        {
            if (action_cb != NULL)
            {
                bool done = action_cb();
                if (done)
                {
                    action_cb = NULL;
                }
            }
            ringloop->submit();
        };
        ringloop->register_consumer(&consumer);
        consumer.loop();
    });
    // Loop until it completes
    while (action_cb != NULL)
    {
        ringloop->loop();
        if (action_cb != NULL)
            ringloop->wait();
    }
}

int main(int narg, const char *args[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    exe_name = args[0];
    cli_tool_t *p = new cli_tool_t();
    p->run(cli_tool_t::parse_args(narg, args));
    return 0;
}
#!/bin/bash -ex

. `dirname $0`/common.sh

dd if=/dev/zero of=./testdata/test_osd1.bin bs=1024 count=1 seek=$((1024*1024-1))
dd if=/dev/zero of=./testdata/test_osd2.bin bs=1024 count=1 seek=$((1024*1024-1))
dd if=/dev/zero of=./testdata/test_osd3.bin bs=1024 count=1 seek=$((1024*1024-1))
dd if=/dev/zero of=./testdata/test_osd4.bin bs=1024 count=1 seek=$((1024*1024-1))
dd if=/dev/zero of=./testdata/test_osd5.bin bs=1024 count=1 seek=$((1024*1024-1))
dd if=/dev/zero of=./testdata/test_osd6.bin bs=1024 count=1 seek=$((1024*1024-1))

build/src/vitastor-osd --osd_num 1 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd1.bin 2>/dev/null) &>./testdata/osd1.log &
OSD1_PID=$!
build/src/vitastor-osd --osd_num 2 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd2.bin 2>/dev/null) &>./testdata/osd2.log &
OSD2_PID=$!
build/src/vitastor-osd --osd_num 3 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd3.bin 2>/dev/null) &>./testdata/osd3.log &
OSD3_PID=$!
build/src/vitastor-osd --osd_num 4 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd4.bin 2>/dev/null) &>./testdata/osd4.log &
OSD4_PID=$!
build/src/vitastor-osd --osd_num 5 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd5.bin 2>/dev/null) &>./testdata/osd5.log &
OSD5_PID=$!
build/src/vitastor-osd --osd_num 6 --bind_address 127.0.0.1 --etcd_address $ETCD_URL $(node mon/simple-offsets.js --format options --device ./testdata/test_osd6.bin 2>/dev/null) &>./testdata/osd6.log &
OSD6_PID=$!

cd mon
npm install
cd ..
node mon/mon-main.js --etcd_url http://$ETCD_URL --etcd_prefix "/vitastor" --verbose 1 &>./testdata/mon.log &
MON_PID=$!

$ETCDCTL put /vitastor/config/pools '{"1":{"name":"testpool","scheme":"replicated","pg_size":2,"pg_minsize":1,"pg_count":16,"failure_domain":"osd"}}'

sleep 2

if ! ($ETCDCTL get /vitastor/config/pgs --print-value-only | jq -s -e '(.[0].items["1"] | map((.osd_set | select(. > 0)) | length == 2) | length) == 16'); then
    format_error "FAILED: 16 PGS NOT CONFIGURED"
fi

if ! ($ETCDCTL get --prefix /vitastor/pg/state/ --print-value-only | jq -s -e '([ .[] | select(.state == ["active"]) ] | length) == 16'); then
    format_error "FAILED: 16 PGS NOT UP"
fi

LD_PRELOAD=libasan.so.5 \
fio -thread -name=test -ioengine=build/src/libfio_vitastor.so -bs=4M -direct=1 -iodepth=1 -fsync=1 -rw=write \
    -etcd=$ETCD_URL -pool=1 -inode=2 -size=128M -cluster_log_level=10

try_change()
{
    n=$1

    $ETCDCTL put /vitastor/config/pools '{"1":{"name":"testpool","scheme":"replicated","pg_size":2,"pg_minsize":2,"pg_count":'$n',"failure_domain":"osd"}}'

    for i in {1..10}; do
        ($ETCDCTL get /vitastor/config/pgs --print-value-only | jq -s -e '(.[0].items["1"] | map((.osd_set | sort) == ["1","2","3"]) | length) == '$n) && \
            ($ETCDCTL get --prefix /vitastor/pg/state/ --print-value-only | jq -s -e '([ .[] | select(.state == ["active"] or .state == ["active", "has_misplaced"]) ] | length) == '$n'') && \
            break
        sleep 1
    done

    # Wait for the rebalance to finish
    for i in {1..60}; do
        ($ETCDCTL get --prefix /vitastor/pg/state/ --print-value-only | jq -s -e '([ .[] | select(.state == ["active"]) ] | length) == '$n'') && \
            break
        sleep 1
    done

    if ! ($ETCDCTL get /vitastor/config/pgs --print-value-only | jq -s -e '(.[0].items["1"] | map((.osd_set | sort) == ["1","2","3"]) | length) == '$n); then
        $ETCDCTL get /vitastor/config/pgs
        $ETCDCTL get --prefix /vitastor/pg/state/
        format_error "FAILED: $n PGS NOT CONFIGURED"
    fi

    if ! ($ETCDCTL get --prefix /vitastor/pg/state/ --print-value-only | jq -s -e '([ .[] | select(.state == ["active"]) ] | length) == '$n); then
        $ETCDCTL get /vitastor/config/pgs
        $ETCDCTL get --prefix /vitastor/pg/state/
        format_error "FAILED: $n PGS NOT UP"
    fi
}

# 16 -> 32

try_change 32

# 32 -> 16

try_change 16

# 16 -> 25

try_change 25

# 25 -> 17

try_change 17

# 17 -> 16

try_change 16

# Monitor should report non-zero overall statistics at least once

if ! (grep /vitastor/stats ./testdata/mon.log | jq -s -e '[ .[] | select((.kv.value.op_stats.primary_write.count | tonumber) > 0) ] | length > 0'); then
    format_error "FAILED: monitor doesn't aggregate stats"
fi

# Changing pg count should never produce the 'has_degraded' object state

if grep has_degraded ./testdata/osd*.log; then
    format_error "FAILED: some objects were degraded during PG move"
fi

format_green OK

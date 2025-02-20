#!/bin/bash


if [ "$#" -ne 1 ]; then
  echo "Usage: ./db_bench.sh [device]"
  exit 1 
fi

DEVICE=$1

rm -rf /tmp/zenfs_$DEVICE*
./build/zenfs mkfs --zbd=$DEVICE --aux_path=/tmp/zenfs_$DEVICE --force=true

./build/db_bench \
        --zbd_path=$DEVICE \
	--benchmarks=fillrandom \
	--use_existing_db=0 \
	--histogram=1 \
	--statistics=0 \
        --stats_per_interval=1 \
        --stats_interval_seconds=60 \
        --max_background_flushes=6 \
	--max_background_compactions=15 \
	--enable_lazy_compaction=0 \
	--level0_file_num_compaction_trigger=4 \
	--sync=1 \
	--allow_concurrent_memtable_write=1 \
	--bytes_per_sync=32768 \
	--wal_bytes_per_sync=32768 \
	--delayed_write_rate=419430400 \
	--enable_write_thread_adaptive_yield=1 \
	--threads=1 \
	--num_levels=7 \
	--key_size=36 \
	--value_size=1111 \
	--level_compaction_dynamic_level_bytes=true \
	--mmap_read=false \
	--compression_type=zstd \
	--memtablerep=skip_list \
	--use_terark_table=false \
	--blob_size=1024 \
	--blob_gc_ratio=0.0625 \
	--write_buffer_size=268435456 \
	--max_write_buffer_number=10 \
	--target_file_size_base=134217728 \
	--target_blob_file_size=134217728 \
	--blob_file_defragment_size=33554432 \
	--max_dependence_blob_overlap=128 \
	--optimize_filters_for_hits=true \
	--optimize_range_deletion=true \
	--num=6000000 \
	--db=test_db_1 \
	--benchmark_write_rate_limit=20971520 \
	--zenfs_gc_ratio=0.3

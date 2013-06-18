#!/bin/bash

clear

# Cache Variables
source_device="/dev/dm-0"
cache_device="/dev/sdc2"
cache_policy="lru"
cache_mode="wt"
cache_block_size="4096"
cache_name="cache1"

# FIO Variables
fio_blocksize="4K"
file_size="20G"
iodepth="8"
numjob="4"
runtime="300"
output_path="/root/eio_perf/dmcache/ram_8G_50G_wt_100_read_0_write_4K_4K"
hits="90"

mkdir -p ${output_path}
echo "Output path '${output_path}' is created"

# Create a cache
echo "Creating a cache"
#eio_cli create -d ${source_device} -s ${cache_device} -p ${cache_policy} -m ${cache_mode} -b ${cache_block_size} -c ${cache_name}
dmsetup create ${cache_name} --table '0 195309568 cache /dev/sdc2 /dev/sdc1 /dev/sdb1 512 1 writethrough default 0'
echo
dmsetup table 	
echo 	

# Warm up the cache
echo "${hit} % Hit_Warm_Up_${fio_blocksize}"
fio --direct=1 --size=${hit}% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=rw --rwmixread=100 --rwmixwrite=0 --iodepth=${iodepth} --filename=${source_device} --name=${hit}_Hit_${fio_blocksize}_WarmUp --output=/tmp/WarmUp.txt

#Run the test
echo "$hit % Hit_${fio_blocksize} ${source_device}"
fio --direct=1 --size=100% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=randrw --rwmixread=90 --rwmixwrite=10 --iodepth=${iodepth} --numjob=${numjob} --group_reporting --filename=${source_device} --name=${hit}_Hit_${fio_blocksize} --random_distribution=zipf:1.2 --output=${output_path}/${hit}_Hit_${fio_blocksize}.txt


# Delete the cache
echo "Deleting the cache"
dmsetup remove ${cache_name}


# Wiping out stale metadata of dm device, if any. Not doing this will cause cache hits during warmup phase on successive
# runs, contrary to our expectation. 
echo "Wiping out cache metadata" 
dd if=/dev/zero of=/dev/sdc2 oflag=direct bs=1M count=1
dd if=/dev/zero of=/dev/sdc1 oflag=direct bs=1M count=1

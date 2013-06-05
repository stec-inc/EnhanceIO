#!/bin/bash

clear

# Cache Variables
source_device="/dev/sdb1"
cache_device="/dev/sdc1"
cache_policy="lru"
cache_mode="wt"
cache_block_size="4096"
cache_name="cache1"

# FIO Variables
fio_blocksize="4K"
file_size="10G"
iodepth="8"
numjob="4"
rread="90"
rwrite="10"
runtime="300"
hits="90"
ram=`free -mto | grep Mem: | awk '{ print $2 "MB" }'`
output_path="/root/eio_perf/eio/${cache_mode}_${rread}_read_${rwrite}_write_${fio_blocksize}_IO_4K_blocksize_${ram}_ram_${hits}_hits"

mkdir -p ${output_path}
echo "Output path '${output_path}' is created"

# Create a cache
echo "Creating a cache"
eio_cli create -d ${source_device} -s ${cache_device} -p ${cache_policy} -m ${cache_mode} -b ${cache_block_size} -c ${cache_name}

# Warm up the cache
echo "${hit} % Hit_Warm_Up_${fio_blocksize}"
fio --direct=1 --size=${hit}% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=rw --rwmixread=100 --rwmixwrite=0 --iodepth=${iodepth} --filename=${source_device} --name=${hit}_Hit_${fio_blocksize}_WarmUp --output=${output_path}/${hit}_Hit_${fio_blocksize}_WarmUp.txt

# Run the test
echo "$hit % Hit_${fio_blocksize} ${source_device}"
fio --direct=1 --size=100% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=randrw --rwmixread=${rread} --rwmixwrite=${rwrite} --iodepth=${iodepth} --numjob=${numjob} --group_reporting --filename=${source_device} --name=${hit}_Hit_${fio_blocksize} --random_distribution=zipf:1.2 --output=${output_path}/${hit}_Hit_${fio_blocksize}.txt


# Delete the cache
echo "Deleting the cache"
eio_cli delete -c ${cache_name}

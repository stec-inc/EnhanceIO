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
file_size="20G"
iodepth="8"
rread=90
rwrite=10
numjob="4"
runtime="300"
hits="90"

# DON'T SET
filename=""

ram=`free -mto | grep Mem: | awk '{ print $2 "MB" }'`

echo $ram
output_path="/root/eio_perf/bcache/${cache_mode}_${rread}_read_${rwrite}_write_${fio_blocksize}_IO_4K_blocksize_${ram}_ram_${hits}_hits"

bcache_create()
{
    echo "Creating a cache"
    make-bcache -B ${source_device}
    make-bcache -C ${cache_device}
    echo ${source_device} > /sys/fs/bcache/register
    echo ${cache_device} > /sys/fs/bcache/register
    uuid=`bcache-super-show -f ${cache_device}  | grep cset.uuid | awk '{ print $2 }'`
    cache=`ls /dev/bcache* | cut -d '/' -f3`
    echo "uuid:$uuid cache:$cache" 
    echo $uuid > /sys/block/$cache/bcache/attach
    dev=`ls ${source_device} | cut -d '/' -f3`
    len=${#dev}
    if [ $len != "4" ]; then
        echo 1 > /sys/block/$dev/bcache/running
    else 
        base=${dev:0:3}
        echo 1 > /sys/block/$base/$dev/bcache/running
    fi   
    if [ $cache_mode = "wb" ]; then
        echo "Setting cache mode: $cache_mode"
        echo writeback > /sys/block/$cache/bcache/cache_mode
    fi    
    if [ $cache_mode = "wa" ]; then
        echo "Setting cache mode: $cache_mode"
        echo writearound > /sys/block/$cache/bcache/cache_mode
    fi    
    echo 0 > /sys/block/$cache/bcache/sequential_cutoff 
    echo "cache $cache created successfully"
    filename="/dev/$cache" 
    return 0   
}
bcache_delete()
{
    echo "Deleting the cache"
    uuid=`bcache-super-show -f ${cache_device}  | grep cset.uuid | awk '{ print $2 }'`
    echo 1 > /sys/fs/bcache/$uuid/unregister
    dev=`ls ${source_device} | cut -d '/' -f3`
    len=${#dev}
    if [ $len != "4" ]; then 
        echo 1 > /sys/block/$dev/bcache/stop
    else 
        base=${dev:0:3} 
        echo 1 > /sys/block/$base/$dev/bcache/stop
    fi   
    echo 1 > /sys/fs/bcache/$uuid/stop  
    echo "cache deleted successfully" 
    sleep 10 
    return 0 
}  
mkdir -p ${output_path}
echo "Output path '${output_path}' is created"

# Create a cache
bcache_create 

echo "running fio on: $filename"
# Warm up the cache
echo "${hit} % Hit_Warm_Up_${fio_blocksize}"
fio --direct=1 --size=${hit}% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=rw --rwmixread=100 --rwmixwrite=0 --iodepth=${iodepth} --filename=${filename} --name=${hit}_Hit_${fio_blocksize}_WarmUp --output=${output_path}/${hit}_Hit_${fio_blocksize}_WarmUp.txt

# Run the test
echo "$hit % Hit_${fio_blocksize} ${iodev}"
#--runtime=${runtime}
fio --direct=1 --size=100% --filesize=${file_size} --blocksize=${fio_blocksize} --ioengine=libaio --rw=randrw --rwmixread=${rread} --rwmixwrite=${rwrite} --iodepth=${iodepth} --numjob=${numjob} --group_reporting --filename=${filename} --name=${hit}_Hit_${fio_blocksize} --random_distribution=zipf:1.2 --output=${output_path}/${hit}_Hit_${fio_blocksize}.txt

# Delete the cache
bcache_delete 




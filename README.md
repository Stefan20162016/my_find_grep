# my_find_grep & copy
## parallel find, grep & copy

parallel or concurrent find grep with O_DIRECT mode for grep
TL;DR:

- grep uses a few kilobytes as buffer and reads files chunkwise, skips files if a '\0' is found and checks if the search string is at the buffer boundary
- threads increment atomic counters to indicate if they are working and might find new directories which are the work-units the threads are working on

============== my find grep: concurrent find respectively concurrent grep ==============

(grep with O_DIRECT mode: (name the executable 'mfgo' or change the code below)

compile:

g++ -O3 -std=c++17 -pthread my_find_grep.cpp -o mfg 

intel compiler:

dpcpp -O3 -std=c++17 -pthread fsscanner_atomic_find_grep.cpp -o fsscanner_atomic_find_grep && time ./fsscanner_atomic_find_grep 2 ~/nvme/code MAINTAINERS grep

usage:
./mfg <number of threads> <path> <search_string> [grep|find|grepCPP|grepCPPI]  # defaults to find

- grep: skipping binary files and reading chunks/buffer with read() syscall
- find: C++ string search on whole file path
- grepCPP: using C++ searches binary files
- grepCPPI: using C++ skipping binary files (like grep -I)

e.g. ./mfg 64 /usr/src/linux  "MAINTAINERS" grep

notes:

grep by default skips binary files (files with \0 in them)

also searching in file path part not just after last  more like find -type f <path> | grep <search_string>

---- advanced notes ----
heuristic: 
- one directory per thread
- no `stat` syscalls to speed up traversal
- array of atomic variables are used to synchronize/wait for other threads which might find new directories
- increment atomic variable in section where we do directory listings and might find new directories
- decrement after we are done in this section
- check global vector with directories for more work
- wait if other threads have a postive atomic variable
- else: die
    - don't wait forever: say 1000 threads are running, but near the end only a few directories are left and ~999 are "busy waiting and checking atomic variables, i.e. waisting cpu time
----

# for more open files
ulimit -n 123456
# for more threads > 32k; I guess 2 mmap's per thread
echo 777888 >  /proc/sys/vm/max_map_count

e.g. use `iostat -x -p nvme0n1p1 --human 1` to find sweet spot
mpstat -P ALL 1
vmstat -m 1  | egrep "dentry|inode_cache" # print slabs
bcc/tools/syscount.py to monitor syscalls

read-ahead settings, scheduler, etc: https://cromwell-intl.com/open-source/performance-tuning/disks.html
https://wiki.mikejung.biz/Ubuntu_Performance_Tuning

echo kyber > /sys/block/nvme0n1/queue/scheduler
echo 512 > /sys/block/nvme0n1/queue/nr_requests
echo 0 > /sys/block/nvme0n1/queue/read_ahead_kb

Benchmarks:

find:
$ time find /nvme -name "*MAINTAINERS*"
15 secs; in-cache: 1.8s
$ time ./mfg 32 threads
1 sec; in-cache: 0.25s

$ time ./mfg 128 /nvme "MAINTAINERS" grep
16-20 sec
minimum: with 209 threads: 11 sec ---> high variance than xargs or ripgrep

# skip binary files:
time (find /nvme/ -type f -print0  | xargs -P64 -0 grep -I MAINTAINERS | wc -l)
 20 sec


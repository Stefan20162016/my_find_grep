#!/bin/bash

if [ ! -e linux-5.13.10.tar.xz ] 
  then 
  wget https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.13.10.tar.xz
  tar xfJ linux-5.13.10.tar.xz
fi


N_DIRS=13

for i in $(seq 1 1 $N_DIRS)
do
        echo $i
        time cp -a linux-5.13.10 linux-$i
done

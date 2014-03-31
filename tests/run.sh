#!/bin/sh
for i in 5.2/*.lua; do
  echo $i
  ../src/luajit $i || exit
done

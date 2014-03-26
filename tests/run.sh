#!/bin/sh
for i in 5.2/*.lua; do
  ../src/luajit $i || exit
done

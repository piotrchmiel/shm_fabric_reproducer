#!/bin/bash

count=0
while ./build/reproducer; do
  ((count++))
  echo "Run #$count succeeded"
done

echo "Failure after $count successful runs"

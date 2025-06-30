#!/bin/bash

git submodule update --init --recursive

num_cpus=$(nproc)

cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -S. -B build
cmake  --build build -- -j "$num_cpus"

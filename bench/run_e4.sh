#!/usr/bin/env bash
# E4: when does a hash join beat a nested loop join? Builds the release preset, runs the
# experiment and saves the CSV. Usage: bench/run_e4.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e4 "$@" | tee bench/results/e4.csv

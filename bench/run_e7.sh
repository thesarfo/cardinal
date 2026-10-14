#!/usr/bin/env bash
# E7: merge join against hash join, with the sort paid for and with the inputs in order from an
# index. Builds the release preset, runs the experiment and saves the CSV.
# Usage: bench/run_e7.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e7 "$@" | tee bench/results/e7.csv

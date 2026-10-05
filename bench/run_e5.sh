#!/usr/bin/env bash
# E5: how wrong do the row-count guesses get? Builds the release preset, runs the experiment
# and saves the CSV. Usage: bench/run_e5.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e5 "$@" | tee bench/results/e5.csv

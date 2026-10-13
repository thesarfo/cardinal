#!/usr/bin/env bash
# E3: how much does join order change the runtime? Builds the release preset, runs the
# experiment and saves the CSV. Usage: bench/run_e3.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e3 "$@" | tee bench/results/e3.csv

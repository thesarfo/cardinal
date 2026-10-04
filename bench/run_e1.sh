#!/usr/bin/env bash
# E1: how much does filter pushdown help? Builds the release preset, runs the experiment
# and saves the CSV. Usage: bench/run_e1.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e1 "$@" | tee bench/results/e1.csv

#!/usr/bin/env bash
# E2: where does an index scan stop paying? Builds the release preset, runs the experiment and
# saves the CSV. Usage: bench/run_e2.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench e2 "$@" | tee bench/results/e2.csv

#!/usr/bin/env bash
# How long the join order search takes, by number of tables. Builds the release preset, runs
# it and saves the CSV. Usage: bench/run_plan.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench plan "$@" | tee bench/results/plan.csv

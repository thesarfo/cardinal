#!/usr/bin/env bash
# Runs the calibration grid on the release build, saves the CSV and prints the fit and the rank
# agreement. Usage: bench/run_calibration.sh [extra cardinal_bench options]
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release
cmake --build --preset release --target cardinal_bench

mkdir -p bench/results
./build/release/bench/cardinal_bench calibrate "$@" > bench/results/calibration.csv
python3 bench/analyze_calibration.py bench/results/calibration.csv

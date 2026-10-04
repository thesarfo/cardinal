#!/usr/bin/env bash
# Random queries through the answer checker: 10,000 per seed, for seeds 1..N (default 20).
# Usage: scripts/fuzz.sh [seeds] [queries-per-seed]
# A failure prints the seed, the query, the shrunk query and which setups disagreed.
set -euo pipefail
cd "$(dirname "$0")/.."

seeds="${1:-20}"
queries="${2:-10000}"

cmake --preset release
cmake --build --preset release --target cardinal_fuzz

for seed in $(seq 1 "$seeds"); do
  ./build/release/tests/cardinal_fuzz "$seed" "$queries"
done

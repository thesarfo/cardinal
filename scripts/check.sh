#!/usr/bin/env bash
# Configure, build and run the tests under the asan preset.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset asan
cmake --build --preset asan
ctest --preset asan

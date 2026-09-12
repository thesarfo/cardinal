#!/usr/bin/env bash
# M2 demo: a table of 100 users, then a query that filters, sorts and limits.
# Build first with: cmake --preset dev && cmake --build --preset dev
set -euo pipefail
cd "$(dirname "$0")/.."
cardinal="${CARDINAL:-build/dev/cardinal}"

{
  echo "CREATE TABLE users (id INT, name TEXT, age INT);"
  seq 1 100 | awk 'BEGIN { printf "INSERT INTO users VALUES " }
                   { printf "%s(%d, '"'"'user%d'"'"', %d)", (NR > 1 ? ", " : ""), $1, $1, 18 + $1 % 40 }
                   END { print ";" }'
  echo "SELECT name, age FROM users WHERE age > 50 ORDER BY age DESC, id LIMIT 5;"
  echo "SELECT id, name FROM users WHERE age BETWEEN 20 AND 21 AND id NOT IN (2, 3, 42) ORDER BY id LIMIT 4;"
} | "$cardinal"

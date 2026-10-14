# cardinal

A small SQL database written to learn how a query optimizer thinks. It reads SQL, works out the cheapest way to answer it, and runs it. The optimizer is the point: it rewrites plans, guesses row counts from statistics, prices each way to run a step, searches for a join order, and can show every decision with `EXPLAIN`. A checker proves it never changes an answer.

## Quickstart

You need a C++20 compiler (GCC 12+ or Clang 15+), CMake 3.25+ and Ninja.

    git clone <this repo> cardinal && cd cardinal
    cmake --preset release && cmake --build --preset release
    ./build/release/cardinal

At the prompt, load the sample tables and ask for the plan and what really happened:

    cardinal> .demo 2000
    cardinal> EXPLAIN ANALYZE VERBOSE SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id
         ...> WHERE u.country = 'Ghana' AND o.amount > 100;

The output has, in order: the plan as written, the rewrite rules that fired, the plan after them, the way each step could run with its cost and the winner marked, the plan chosen, and the guessed row count against the real one for every step.

Try these next:

    cardinal> CREATE INDEX orders_user ON orders (user_id);
    cardinal> EXPLAIN VERBOSE SELECT * FROM orders WHERE user_id = 7;
    cardinal> SELECT country, COUNT(*), AVG(age) FROM users GROUP BY country ORDER BY country;
    cardinal> .stats users

Shell commands: `.demo [users]`, `.stats table`, `.bushy on|off` (join order search), `.indexes on|off`, `.quit`.

## Tests

    cmake --preset dev && cmake --build --preset dev
    ctest --preset dev

`cmake --preset asan` builds with the address and undefined-behavior sanitizers. `build/dev/tests/cardinal_fuzz <seed> <count>` runs random queries through the answer checker.

## Experiments

`bench/run_e1.sh` through `bench/run_e7.sh` rebuild their data, time it in a release build and write a CSV that names the compiler, flags, CPU and commit. The write-ups are in `docs/benchmarks.md`.

## Where to read

| | |
|---|---|
| `docs/architecture.md` | the pipeline and the source tree |
| `docs/optimizer.md` | the rules, join order search, grouping, indexes, output order |
| `docs/cardinality-estimation.md` | statistics and how row counts are guessed |
| `docs/cost-model.md` | the formulas and how the constants were fitted |
| `docs/indexes.md` | the B+ tree and index scans |
| `docs/execution-engine.md` | the operators |
| `docs/benchmarks.md` | experiments E1 to E7, with data |
| `docs/decisions.md` | what was chosen and what was passed on |
| `docs/status.md` | what is built, what is not |

#!/usr/bin/env python3
"""Run the tests/slt/*.test files against SQLite and compare with what cardinal expects.

The .test files hold the answers cardinal gives (and the test runner checks them). This
script loads the same data into SQLite, runs the same queries, and checks SQLite gives the
same answers. A record where the two differ must be listed in tests/slt/sqlite_exclusions.txt
with the reason; a difference that is not listed is a failure, and so is a listed record
that now agrees (the reason has gone stale).

Usage: scripts/compare_sqlite.py [--list] [file.test ...]
  --list   also print every excluded record with its reason
"""

import glob
import os
import re
import sqlite3
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXCLUSIONS = os.path.join(ROOT, "tests", "slt", "sqlite_exclusions.txt")


def normalize(sql):
    return re.sub(r"\s+", " ", sql.strip()).rstrip(";").strip()


def read_exclusions():
    """Lines of `file | sql | reason`. `*` as the file means any file."""
    out = {}
    if not os.path.exists(EXCLUSIONS):
        return out
    for number, line in enumerate(open(EXCLUSIONS), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(" | ", 2)]
        if len(parts) != 3 or not parts[2]:
            sys.exit(f"{EXCLUSIONS}:{number}: expected `file | sql | reason`, with a reason")
        out[(parts[0], normalize(parts[1]))] = {"reason": parts[2], "used": False, "line": number}
    return out


def read_records(path):
    """Yield (line_number, kind, header, sql, expected_lines) for each record in a .test file."""
    lines = open(path).read().split("\n")
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        start = i + 1
        i += 1
        if not line or line.startswith("#"):
            continue
        sql = []
        while i < len(lines) and lines[i].strip() and lines[i].rstrip() != "----":
            sql.append(lines[i].rstrip())
            i += 1
        expected = []
        if i < len(lines) and lines[i].rstrip() == "----":
            i += 1
            while i < len(lines) and lines[i].strip():
                expected.append(lines[i].rstrip())
                i += 1
        kind = line.split()[0]
        yield start, kind, line, "\n".join(sql), expected


def show(value, letter):
    """One value as a .test file spells it, or None if it does not fit the declared letter."""
    if value is None:
        return "NULL"
    if letter == "I":
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, int):
            return str(value)
    elif letter == "R":
        if isinstance(value, (int, float)):
            return "%.3f" % value
    elif letter == "T":
        if isinstance(value, str):
            return value if value else "(empty)"
    return None


def run_query(db, header, sql):
    words = header.split()
    types = words[1] if len(words) > 1 else ""
    mode = words[2] if len(words) > 2 else "nosort"
    rows = db.execute(sql).fetchall()
    shown = []
    for row in rows:
        if len(row) != len(types):
            return None, f"declared {len(types)} columns, SQLite returned {len(row)}"
        cells = []
        for value, letter in zip(row, types):
            text = show(value, letter)
            if text is None:
                return None, f"column declared {letter} but SQLite returned {value!r}"
            cells.append(text)
        shown.append(cells)
    if mode == "rowsort":
        shown.sort()
    flat = [c for row in shown for c in row]
    if mode == "valuesort":
        flat.sort()
    return flat, None


def check_file(path, exclusions, report):
    name = os.path.basename(path)
    db = sqlite3.connect(":memory:", isolation_level=None)
    agreed = excluded = 0
    for line, kind, header, sql, expected in read_records(path):
        key_exact = (name, normalize(sql))
        key_any = ("*", normalize(sql))
        problem = None

        if kind == "statement" and header.split()[1] == "ok":
            try:
                db.execute(sql)
            except sqlite3.Error as e:
                problem = f"SQLite failed: {e}"
        elif kind == "statement":
            # cardinal says this fails and changes nothing. Run it, then undo it either way.
            db.execute("SAVEPOINT probe")
            try:
                db.execute(sql)
                problem = "cardinal rejects this and SQLite accepts it"
            except sqlite3.Error:
                pass
            db.execute("ROLLBACK TO probe")
            db.execute("RELEASE probe")
        elif kind == "query":
            try:
                got, why = run_query(db, header, sql)
            except sqlite3.Error as e:
                got, why = None, f"SQLite failed: {e}"
            if why:
                problem = why
            elif got != expected:
                problem = f"expected {expected}, SQLite gave {got}"
        else:
            problem = f"unknown record type {kind}"

        entry = exclusions.get(key_exact) or exclusions.get(key_any)
        if problem is None:
            if entry:
                entry["used"] = "agrees"
            agreed += 1
        elif entry:
            entry["used"] = entry["used"] or True
            excluded += 1
            report["excluded"].append((name, line, normalize(sql), entry["reason"], problem))
        else:
            report["failed"].append((name, line, normalize(sql), problem))
    return agreed, excluded


def main():
    args = sys.argv[1:]
    show_list = "--list" in args
    files = [a for a in args if not a.startswith("--")] or sorted(glob.glob(os.path.join(ROOT, "tests", "slt", "*.test")))
    exclusions = read_exclusions()
    report = {"failed": [], "excluded": []}
    total_agreed = total_excluded = 0

    print(f"SQLite {sqlite3.sqlite_version}")
    for path in files:
        agreed, excluded = check_file(path, exclusions, report)
        total_agreed += agreed
        total_excluded += excluded
        print(f"  {os.path.basename(path)}: {agreed} agree, {excluded} excluded")

    if show_list:
        print("\nExcluded (cardinal and SQLite differ on purpose):")
        for name, line, sql, reason, problem in report["excluded"]:
            print(f"  {name}:{line}  {sql}\n      why: {reason}")

    stale = []
    for (name, sql), entry in exclusions.items():
        if name != "*" and not any(os.path.basename(f) == name for f in files):
            continue  # that file was not run this time
        if entry["used"] is False or entry["used"] == "agrees":
            stale.append((name, sql, entry))

    status = 0
    if report["failed"]:
        status = 1
        print("\nDIFFERENCES with no written reason:")
        for name, line, sql, problem in report["failed"]:
            print(f"  {name}:{line}  {sql}\n      {problem}")
    if stale:
        status = 1
        print("\nExclusions that no longer apply (the record agrees, or is gone). Remove them:")
        for name, sql, entry in stale:
            print(f"  {os.path.basename(EXCLUSIONS)}:{entry['line']}  {name} | {sql}")

    print(f"\n{total_agreed} records agree with SQLite, {total_excluded} differ for a written reason, "
          f"{len(report['failed'])} differ without one.")
    sys.exit(status)


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# Run the complete LinuxLab test suite.
#
#   scripts/run_tests.sh [bin-dir]      (default: build/bin)
#
# Suites:
#   unit    lexer/parser unit tests (C)
#   shell   lsh integration tests   (bash, runs lsh -c ...)
#   jobctl  interactive job control on a pseudo-terminal (python3)
#   labs    smoke tests for every lab, tool and benchmark
set -u
cd "$(dirname "$0")/.." || exit 2
BIN=${1:-build/bin}

failed=()
run_suite() {
    local name=$1
    shift
    echo "================================================================ $name"
    if "$@"; then
        echo "---- $name: PASS"
    else
        echo "---- $name: FAIL"
        failed+=("$name")
    fi
    echo
}

run_suite unit  "$BIN/test_parser"
run_suite shell bash tests/shell/test_shell.sh "$BIN/lsh"
if command -v python3 >/dev/null; then
    run_suite jobctl python3 tests/shell/test_job_control.py "$BIN/lsh"
else
    echo "(python3 not found: skipping the pty job-control tests)"
fi
run_suite labs  bash tests/labs/test_labs.sh "$BIN"

if [ ${#failed[@]} -eq 0 ]; then
    echo "ALL SUITES PASSED"
    exit 0
fi
echo "FAILED SUITES: ${failed[*]}"
exit 1

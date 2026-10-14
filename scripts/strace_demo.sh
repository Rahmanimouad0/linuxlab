#!/usr/bin/env bash
# Reproduce the traces used in docs/syscall-tracing.md.
#
#   scripts/strace_demo.sh [path/to/lsh]
#
# Requires strace (sudo apt install strace). Output goes to traces/.
set -eu
cd "$(dirname "$0")/.."
LSH=$(realpath "${1:-build/bin/lsh}")
command -v strace >/dev/null || { echo "strace not installed" >&2; exit 1; }

mkdir -p traces
cd traces
printf 'hello\nworld\n' > test.txt

echo ">> 1. lsh -c 'cat test.txt'            -> traces/cat.txt"
strace -f -o cat.txt "$LSH" -c 'cat test.txt' > /dev/null

echo ">> 2. a pipeline with a redirection    -> traces/pipeline.txt"
strace -f -o pipeline.txt -e trace=pipe2,clone,clone3,dup2,close,openat,execve,wait4,exit_group \
    "$LSH" -c 'cat test.txt | grep o > out.txt'

echo ">> 3. syscall counts for one command"
strace -c -f "$LSH" -c 'cat test.txt' 2>&1 > /dev/null | head -n 20

echo
echo "Tips:"
echo "  grep -E 'clone|execve|dup2|pipe2|wait4' traces/pipeline.txt   # the shell's part"
echo "  strace -f -e trace=setpgid,ioctl $LSH                          # job control, interactive"

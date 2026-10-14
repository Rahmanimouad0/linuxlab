#!/usr/bin/env bash
# Smoke tests for the labs, tools and benchmarks: each program must exit 0
# and print the result that demonstrates its point.
#
# usage: tests/labs/test_labs.sh [bin-dir]
set -u

BIN=$(realpath "${1:-build/bin}")
pass=0
fail=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2

# check NAME REGEX COMMAND [ARGS...]
check() {
    local name=$1 re=$2 out status
    shift 2
    out=$(timeout 120 "$@" 2>&1)
    status=$?
    if [ $status -ne 0 ]; then
        fail=$((fail + 1))
        printf 'FAIL  %s: exit status %d\n' "$name" "$status"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
    elif ! grep -Eq -- "$re" <<<"$out"; then
        fail=$((fail + 1))
        printf 'FAIL  %s: output does not match /%s/\n' "$name" "$re"
        printf '%s\n' "$out" | tail -n 5 | sed 's/^/      /'
    else
        pass=$((pass + 1))
        printf 'ok    %s\n' "$name"
    fi
}

echo "== processes"
check "fork returns 0 in the child"      'fork\(\) returned 0 '            "$BIN/fork_lab" 20
check "stdio buffer duplicated"          '"hello hello "'                  "$BIN/fork_lab" 20
check "exit(256) is seen as 0"           'exited, code 7'                  "$BIN/fork_lab" 20
check "zombie state observed"            ' Z  zombie'                      "$BIN/proc_states_lab"
check "zombie gone after wait"           'no longer exists'                "$BIN/proc_states_lab"
check "subreaper adopts orphan"          'now [0-9]+ \(proc_states_lab\)'  "$BIN/proc_states_lab"

echo "== signals"
check "standard signals coalesce"        'after unblocking, handler ran 1 time' "$BIN/signal_lab"
check "real-time signals queue"          'SIGRTMIN queued 5x while blocked; handler ran 5 time' "$BIN/signal_lab"
check "SIGKILL cannot be caught"         'sigaction\(SIGKILL\) -> -1'      "$BIN/signal_lab"
check "SIGTERM allows clean exit"        'sent SIGTERM: +exited normally, code 0' "$BIN/signal_lab"

echo "== file descriptors"
check "dup shares the offset"            '"hello world\\n"'                "$BIN/fd_lab"
check "dup2 redirects stdout"            'printed with plain printf'       "$BIN/fd_lab"
check "O_CLOEXEC fd not inherited"       'inherited\.txt'                  "$BIN/fd_lab"

echo "== ipc"
check "pipe EOF needs all writers closed" 'would block forever'            "$BIN/pipe_lab"
check "pipe small writes are atomic"     '4096 bytes: +[0-9]+ records read, +0 mixed' "$BIN/pipe_lab"
check "fifo transfers data"              'reader got: third message'       "$BIN/fifo_lab"
check "shm + shared mutex is exact"      'with shared mutex  : 2000000'    "$BIN/shm_lab"
check "unix socket echo"                 'got "HELLO"'                     "$BIN/socket_lab"
check "fd passing with SCM_RIGHTS"       'received fd [0-9]+'              "$BIN/socket_lab"

echo "== concurrency"
check "synchronized counters are exact"  'mutex +800000 +0 '              "$BIN/race_lab" -t 4 -n 200000 -r 2 --check
check "producer/consumer checksum"       'checksum   : OK'                 "$BIN/prodcons_lab" -p 3 -c 2 -n 20000 -b 4

echo "== memory"
check "lazy allocation visible in RSS"   'after writing everything'        "$BIN/mem_lab" 64
check "copy-on-write page faults"        'child wrote [0-9]+ pages: +[0-9]+ page faults' "$BIN/mem_lab" 64

echo "== tools"
check "llps lists processes"             'PID +PPID +USER'                 "$BIN/llps"
check "llps sees itself"                 'llps'                            "$BIN/llps"
check "llps detail view"                 "state +[A-Z]"                    "$BIN/llps" -p $$
check "llps tree"                        '(├|└)── .*\([0-9]+\)'            "$BIN/llps" -f
check "llps top view"                    'CPU% +MEM%'                      "$BIN/llps" -t -i 100 -n 3

mkdir -p t/src t/docs
touch t/README.md t/src/main.c t/src/shell.c t/.hidden
expected=$'t\n├── README.md\n├── docs/\n└── src/\n    ├── main.c\n    └── shell.c\n\n2 directories, 3 files'
got=$("$BIN/llfs" tree t 2>&1)
if [ "$got" == "$expected" ]; then
    pass=$((pass + 1)); echo "ok    llfs tree output is exact"
else
    fail=$((fail + 1)); printf 'FAIL  llfs tree\n%s\n' "$got"
fi
check "llfs tree -a shows hidden"        '\.hidden'                        "$BIN/llfs" tree -a t
echo x > perm.txt && chmod 0640 perm.txt
check "llfs perms explains owner bits"   'owner: +rw- '                    "$BIN/llfs" perms perm.txt
check "llfs perms picks the owner class" 'only the OWNER bits apply'       "$BIN/llfs" perms perm.txt
check "llfs stat"                        'Mode: 0640 \(-rw-r-----\)'       "$BIN/llfs" stat perm.txt
check "llfs umask"                       'new file'                        "$BIN/llfs" umask

echo "== benchmarks (quick mode)"
for b in bench_spawn bench_ipc bench_sync bench_fileio; do
    check "$b runs"                      '^benchmark,variant,parameter,value,unit' "$BIN/$b" --csv --quick
done

echo
echo "lab/tool smoke tests: $((pass + fail)) run, $fail failed"
[ $fail -eq 0 ]

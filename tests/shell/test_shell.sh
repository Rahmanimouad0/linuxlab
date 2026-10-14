#!/usr/bin/env bash
# Integration tests for lsh: run command lines with `lsh -c` and compare
# stdout+stderr and the exit status with what a POSIX shell would produce.
#
# usage: tests/shell/test_shell.sh [path/to/lsh]
set -u

LSH=$(realpath "${1:-build/bin/lsh}")
[ -x "$LSH" ] || { echo "lsh not found: $LSH" >&2; exit 2; }

pass=0
fail=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2
WORK=$(pwd -P)

# expect NAME EXPECTED_OUTPUT COMMAND_LINE
expect() {
    local name=$1 want=$2 line=$3 got
    got=$(timeout 10 "$LSH" -c "$line" 2>&1)
    if [ "$got" == "$want" ]; then
        pass=$((pass + 1))
        printf 'ok    %s\n' "$name"
    else
        fail=$((fail + 1))
        printf 'FAIL  %s\n      command:  %s\n      expected: %q\n      got:      %q\n' \
            "$name" "$line" "$want" "$got"
    fi
}

# expect_status NAME EXPECTED_STATUS COMMAND_LINE
expect_status() {
    local name=$1 want=$2 line=$3 got
    timeout 10 "$LSH" -c "$line" >/dev/null 2>&1
    got=$?
    if [ "$got" == "$want" ]; then
        pass=$((pass + 1))
        printf 'ok    %s\n' "$name"
    else
        fail=$((fail + 1))
        printf 'FAIL  %s\n      command: %s\n      expected status %s, got %s\n' \
            "$name" "$line" "$want" "$got"
    fi
}

# expect_match NAME REGEX COMMAND_LINE   (output must match an extended regex)
expect_match() {
    local name=$1 re=$2 line=$3 got
    got=$(timeout 10 "$LSH" -c "$line" 2>&1)
    if [[ "$got" =~ $re ]]; then
        pass=$((pass + 1))
        printf 'ok    %s\n' "$name"
    else
        fail=$((fail + 1))
        printf 'FAIL  %s\n      command: %s\n      regex:   %s\n      got:     %q\n' \
            "$name" "$line" "$re" "$got"
    fi
}

echo "== basics"
expect "simple command"            "hello world"   'echo hello world'
expect "external program + args"   "3"             'expr 1 + 2'
expect "sequence ;"                $'1\n2'         'echo 1; echo 2'
expect "comment ignored"           "a"             'echo a # not printed'
expect "empty line"                ""              '   '
expect "command not found"         $'lsh: nosuchcmd_xyz: command not found\n127' 'nosuchcmd_xyz; echo $?'
expect "not executable"            $'lsh: ./f: Permission denied\n126' 'echo x > f; ./f; echo $?'
expect "absolute path"             "abs"           '/bin/echo abs'

echo "== quoting and expansion"
expect "single quotes"             "a  b|c"        "echo 'a  b|c'"
expect "double quotes"             "a  b;c"        'echo "a  b;c"'
expect "backslash escapes"         'a b x"y'       'echo a\ b "x\"y"'
expect "adjacent quoted parts"     "its"           "echo 'it''s'"
expect "variable expansion"        "bar barbaz bar" 'export FOO=bar; echo $FOO ${FOO}baz "$FOO"'
expect "no expansion in ''"        '$FOO'          "export FOO=bar; echo '\$FOO'"
expect "unset"                     "[]"            'export FOO=1; unset FOO; echo "[$FOO]"'
expect "empty unquoted vanishes"   "[x]"           'printf "[%s]" $NOPE_NOT_SET x'
expect "empty quoted stays"        "[][x]"         'printf "[%s]" "$NOPE_NOT_SET" x'
expect "no word splitting"         "[a b]"         'export X="a b"; printf "[%s]" $X'
expect "tilde"                     "$HOME/x"       'echo ~/x'
expect_match "\$\$ is a pid"       '^[0-9]+$'      'echo $$'
expect "lone dollar"               '$ a$'          'echo $ a$'

echo "== exit status"
expect "\$? after true/false"      $'0\n1'         'true; echo $?; false; echo $?'
expect "\$? is evaluated late"     "1"             'false; echo $?'
expect "&&"                        "yes"           'true && echo yes'
expect "&& short-circuits"         ""              'false && echo yes'
expect "||"                        "no"            'false || echo no'
expect "&& || chain"               "no"            'false && echo yes || echo no'
expect "|| && chain"               "b"             'true || echo a && echo b'
expect "pipeline status = last"    $'0\n1'         'false | true; echo $?; true | false; echo $?'
expect "killed by signal -> 128+n" "143"           'sh -c "kill -TERM \$\$"; echo $?'
expect_status "exit code"          42              'exit 42'
expect_status "exit uses \$?"      1               'false; exit'
expect_status "exit 256 wraps"     0               'exit 256'
expect_status "status of last cmd" 3               'sh -c "exit 3"'

echo "== pipelines"
expect "two stages"                "HELLO"         'echo hello | tr a-z A-Z'
expect "three stages"              "2"             'printf "x\ny\nx\n" | grep x | wc -l'
expect "sort | head"               $'a\nb'         'printf "c\nb\na\n" | sort | head -n 2'
expect "large data, no deadlock"   "100000"        'seq 1 100000 | sort -n | tail -n 1'
expect "EOF propagates"            "x"             'echo x | cat | cat | cat | cat'
expect "builtin in pipeline"       "$WORK"         'pwd | cat'
expect "exit in pipeline is local" "still here"    'exit 3 | true; echo still here'
expect "SIGPIPE ends producer"     "y"             'yes | head -n 1'

echo "== redirection"
expect "> creates/truncates"       "second"        'echo first > o; echo second > o; cat o'
expect ">> appends"                $'a\nb'         'echo a > o; echo b >> o; cat o'
expect "< input"                   "data"          'echo data > in; cat < in'
expect "redirect before command"   "hi"            '> o echo hi; cat o'
expect "2> stderr"                 $'1\nyes'       'cat /nonexistent 2> err; echo $?; test -s err && echo yes'
expect "2>> appends stderr"        "2"             'cat /nx 2> e; cat /nx 2>> e; wc -l < e'
expect "2>&1 into pipe"            "1"             'cat /nonexistent 2>&1 | grep -c "No such file"'
expect "order: >f 2>&1"            "1"             'cat /nonexistent > f 2>&1; grep -c "No such" f'
expect "order: 2>&1 >f"            $'1\n0'         'cat /nonexistent 2>&1 > f | grep -c "No such"; wc -c < f'
expect "close fd with >&-"         "ok"            'echo lost >&- 2>/dev/null; echo ok'
expect "missing input file"        $'lsh: missing: No such file or directory\n1' 'cat < missing; echo $?'
expect "redirect-only creates"     "ok"            '> created; test -f created && echo ok'
expect "last > wins"               $'0\nhi'        'echo hi > a > b; wc -c < a; cat b'
expect "builtin output restored"   $'after\n'"$WORK" 'pwd > p; echo after; cat p'
touch ref_mode
expect "new file mode = 0666&~umask" "$(ls -l ref_mode | cut -c1-10)" 'echo x > m; ls -l m | cut -c1-10'

echo "== builtins"
expect "cd + pwd"                  "/"             'cd /; pwd'
expect "cd with no args -> HOME"   "$HOME"         'cd; pwd'
expect "cd - prints and returns"   $'/\n/'         'cd /; cd /tmp; cd - ; pwd'
expect "cd error"                  $'lsh: cd: /nonexistent: No such file or directory\n1' 'cd /nonexistent; echo $?'
expect "cd too many args"          "lsh: cd: too many arguments" 'cd a b'
expect "cd in a pipeline is lost"  "$WORK"         'cd / | true; pwd'
expect "PWD/OLDPWD updated"        "/tmp $WORK"    'cd /tmp; echo $PWD $OLDPWD'
expect "export invalid name"       "lsh: export: \`1x=2': not a valid identifier" 'export 1x=2'
expect "export visible to child"   "v"             'export LSH_T=v; sh -c "echo \$LSH_T"'
expect_match "help lists cd"       'cd \[dir'      'help'
expect "exit non-numeric"          "lsh: exit: abc: numeric argument required" 'exit abc'

echo "== background jobs"
expect "& returns immediately"     "started"       'sleep 2 & echo started; kill %1'
expect_match "jobs lists job"      '^\[1\]\+ +Running +sleep 1 &$' 'sleep 1 & jobs; kill %1'
expect "wait for all"              $'started\ndone' 'sleep 0.2 & echo started; wait; echo done'
expect "wait %n returns status"    "5"             'sh -c "exit 5" & wait %1; echo $?'
expect "kill %n -> 143"            "143"           'sleep 5 & kill %1; wait %1; echo $?'
expect "kill -KILL -> 137"         "137"           'sleep 5 & kill -KILL %1; wait %1; echo $?'
expect "kill -l"                   " 9) SIGKILL"   'kill -l | grep KILL'
expect "bg job cannot read stdin"  "0"             'cat & wait %1; echo $?'
expect "fg needs job control"      "lsh: fg: no job control" 'fg'
expect "no such job"               "lsh: wait: %7: no such job" 'wait %7'

echo "== syntax errors"
expect "leading pipe"              "lsh: syntax error near unexpected token \`|'" '| ls'
expect "trailing pipe"             "lsh: syntax error near unexpected token \`newline'" 'ls |'
expect "double ;"                  "lsh: syntax error near unexpected token \`;'" 'echo a ;; echo b'
expect "missing redirect target"   "lsh: syntax error near unexpected token \`newline'" 'echo >'
expect "unterminated quote"        "lsh: unterminated single quote" "echo 'abc"
expect_status "syntax error = 2"   2               'echo a |'

echo "== script mode"
cat > script.lsh <<'EOF'
#!/usr/bin/env lsh
# a comment line
export GREETING=hi
echo "$GREETING from a script"
false
echo "status was $?"
exit 7
EOF
got=$(timeout 10 "$LSH" script.lsh 2>&1); status=$?
if [ "$got" == $'hi from a script\nstatus was 1' ] && [ $status -eq 7 ]; then
    pass=$((pass + 1)); echo "ok    script file"
else
    fail=$((fail + 1)); echo "FAIL  script file: got $(printf %q "$got"), status $status"
fi

got=$(printf 'echo from stdin\ncd /\npwd\n' | timeout 10 "$LSH" 2>&1)
if [ "$got" == $'from stdin\n/' ]; then
    pass=$((pass + 1)); echo "ok    commands from a pipe"
else
    fail=$((fail + 1)); echo "FAIL  commands from a pipe: got $(printf %q "$got")"
fi

echo
echo "shell integration tests: $((pass + fail)) run, $fail failed"
[ $fail -eq 0 ]

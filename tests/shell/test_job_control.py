#!/usr/bin/env python3
"""
Interactive job-control tests for lsh.

Job control only exists when the shell runs on a terminal, so these tests
start lsh on a pseudo-terminal (pty) and type into it like a user would:
Ctrl+Z (\\x1a), Ctrl+C (\\x03), fg, bg, jobs.

usage: tests/shell/test_job_control.py [path/to/lsh]
"""
import os
import pty
import re
import select
import sys
import time

LSH = os.path.realpath(sys.argv[1] if len(sys.argv) > 1 else "build/bin/lsh")
PROMPT = re.compile(rb"\$ $")
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")

passed = failed = 0


class Terminal:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["HOME"] = "/tmp"
            os.execv(LSH, [LSH])
        self.buf = b""

    def read_until(self, pattern, timeout=5.0):
        """Read until `pattern` (regex bytes) matches; return the text read."""
        deadline = time.time() + timeout
        while True:
            clean = ANSI.sub(b"", self.buf)
            m = re.search(pattern, clean)
            if m:
                self.buf = b""
                return clean[: m.end()].decode(errors="replace")
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"waiting for {pattern!r}, got {clean[-300:]!r}")
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except OSError:
                    raise TimeoutError("terminal closed")

    def send(self, data):
        os.write(self.fd, data.encode() if isinstance(data, str) else data)

    def command(self, line, expect=PROMPT, timeout=5.0):
        self.send(line + "\n")
        return self.read_until(expect, timeout)

    def close(self):
        try:
            self.send("exit\n")
            time.sleep(0.2)
        except OSError:
            pass
        try:
            os.kill(self.pid, 9)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)


def test(name):
    def wrap(fn):
        global passed, failed
        term = Terminal()
        try:
            term.read_until(PROMPT)
            fn(term)
            passed += 1
            print(f"ok    {name}")
        except (AssertionError, TimeoutError) as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
        finally:
            term.close()
        return fn
    return wrap


@test("prompt and a simple command")
def _(t):
    out = t.command("echo interactive")
    assert "interactive" in out, out


@test("Ctrl+C kills the foreground job, not the shell")
def _(t):
    t.send("sleep 30\n")
    time.sleep(0.3)
    t.send(b"\x03")
    t.read_until(PROMPT)
    out = t.command("echo $?")
    assert "130" in out, out
    out = t.command("echo still alive")
    assert "still alive" in out, out


@test("Ctrl+C at the prompt discards the line")
def _(t):
    t.send("echo should-not-run")
    t.send(b"\x03")
    t.read_until(PROMPT)
    out = t.command("echo next")
    assert "should-not-run\r\n" not in out and "next" in out, out


@test("Ctrl+Z stops the job; jobs shows it")
def _(t):
    t.send("sleep 30\n")
    time.sleep(0.3)
    t.send(b"\x1a")
    out = t.read_until(PROMPT)
    assert re.search(r"\[1\]\+\s+Stopped\s+sleep 30", out), out
    out = t.command("jobs")
    assert "Stopped" in out, out
    t.command("kill %1")


@test("bg resumes a stopped job in the background")
def _(t):
    t.send("sleep 30\n")
    time.sleep(0.3)
    t.send(b"\x1a")
    t.read_until(PROMPT)
    out = t.command("bg")
    assert "[1]+ sleep 30 &" in out, out
    out = t.command("jobs")
    assert re.search(r"Running\s+sleep 30 &", out), out
    t.command("kill %1")


@test("fg brings a stopped job back and waits for it")
def _(t):
    t.send("sleep 1\n")
    time.sleep(0.3)
    t.send(b"\x1a")
    t.read_until(PROMPT)
    start = time.time()
    out = t.command("fg", timeout=5)
    assert "sleep 1" in out, out
    assert time.time() - start > 0.3, "fg returned before the job finished"
    out = t.command("echo $?")
    assert "\r\n0\r\n" in out, out


@test("a stopped pipeline stops as a whole")
def _(t):
    t.send("sleep 30 | cat\n")
    time.sleep(0.3)
    t.send(b"\x1a")
    out = t.read_until(PROMPT)
    assert re.search(r"Stopped\s+sleep 30 \| cat", out), out
    t.command("kill %1")


@test("background job prints [n] pid and later Done")
def _(t):
    out = t.command("sleep 0.2 &")
    assert re.search(r"\[1\] \d+", out), out
    time.sleep(0.5)
    out = t.command("true")
    assert re.search(r"\[1\]\+\s+Done\s+sleep 0\.2", out), out


@test("exit warns once about stopped jobs")
def _(t):
    t.send("sleep 30\n")
    time.sleep(0.3)
    t.send(b"\x1a")
    t.read_until(PROMPT)
    out = t.command("exit")
    assert "There are stopped jobs." in out, out
    t.command("kill %1")


print(f"\njob control tests: {passed + failed} run, {failed} failed")
sys.exit(1 if failed else 0)

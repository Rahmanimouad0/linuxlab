# Contributing

## Build and test

```bash
make -j"$(nproc)"          # or: cmake -B build && cmake --build build -j
make test                  # all suites; or: ctest --test-dir build --output-on-failure
make SAN=1 BUILD=build-san && scripts/run_tests.sh build-san/bin    # ASan + UBSan
```

A change is ready when:

1. it compiles without warnings with both `gcc` and `clang` (`make CC=clang`);
2. `make test` passes, and so does the sanitizer build;
3. `clang-format --dry-run --Werror` reports nothing (`make format` fixes it);
4. new shell behaviour has a test in `tests/shell/test_shell.sh` (or, for anything that needs a
   terminal, in `tests/shell/test_job_control.py`); a new lab has a smoke test in
   `tests/labs/test_labs.sh` that checks the output that *demonstrates* its point.

## Conventions

- C11 plus POSIX/GNU extensions (`_GNU_SOURCE`); Linux only.
- Every descriptor is opened with `O_CLOEXEC` unless it is meant to be inherited.
- Signal handlers only set `volatile sig_atomic_t` flags.
- Check every syscall that can fail; labs may `die_errno()`, the shell must report and continue.
- Numbers in the documentation must come from a real run. Put the command next to them.

## Adding a lab

1. Create `labs/<topic>/<name>_lab.c`. The Makefile and CMake pick it up automatically.
2. Start the file with a comment that lists the experiments, and print a `section()` per
   experiment.
3. Explain what the output means in the matching `docs/*.md` file, quoting real output.

# printf-test

Test runner for `ft_printf` (`bash + cc + valgrind`)

## Run

```sh
cd printf-test
./run.sh
```

The script builds `libftprintf.a` via the project's own Makefile, then
links `tests.c` against it. Each test calls `ft_printf` and libc `printf`
with the same arguments, captures each one's stdout to a temp file, and
compares the bytes byte-for-byte and the integer return value. Any
mismatch fails the test.

## Configuration

Override via env:

```sh
PRINTF_DIR=/path/to/printf ./run.sh
```

Defaults:
- `PRINTF_DIR=../printf`
- `CFLAGS="-Wall -Wextra -Werror -g -O0"`

## Cleaning

```sh
./run.sh --clean    # removes printf-test/build/
```

## What it tests

~160 tests covering:

- **Mandatory conversions** `%c %s %p %d %i %u %x %X %%`: zero/negative/MAX
  values, NULL string, INT_MIN/MAX, UINT_MAX, embedded `%`, long strings,
  NULL/-1/boundary pointers, char overflow (`'0' + 256`), null byte inside
  output, wrong-sized integer args (long passed to `%d`) — catches
  `va_arg(ap, long)` bugs.
- **Bonus flags** `- 0 . # + ' '` and width, with all relevant
  conversions: `%5d`, `%-5s`, `%05d`, `%.5d`, `%.0d` with 0, `%.s`,
  `%#x`, `% d`, `%+d`, plus combinations like `%-10.5d`, `%+05d`,
  `%-#10x`, `% +d`.
- **Stress**: `%500d`, `%.500d`, `%.500s` — catches impls with fixed-size
  internal buffers; long `%d%d…` chains stress va_list traversal.
- **Explicit return-value checks** (`ASSERT_FT_RET`) — independent of libc,
  guarantees the integer-return contract holds.

## Harness guarantees

- **Self-test on startup**: validates capture, truncation detection, AND
  the fork-path end-to-end (a deliberately failing test must produce
  `TR_FAIL`, a no-op test must produce `TR_PASS`). Aborts with
  `FATAL: …` before any user test runs if any check fails.
- **No silent truncation**: if captured output overflows `CAP_BUF`
  (8 KiB), the test fails loudly with `output exceeded CAP_BUF — raise
  it` instead of comparing truncated buffers.
- **Per-test isolation**: each test runs in a forked child with a 5 s
  timeout (`alarm()` + blocking `waitpid`, no busy-poll). A crash or
  hang doesn't poison the next test.
- **Always emits a result line**: PASS / FAIL / TIMEOUT / CRASH — the
  test name always appears in stdout, even on failure.
- **Format-checker stays on** for normal tests; only the handful of
  deliberately-broken format strings (long passed to `%d`, redundant
  flags like `%-05d`, `% +d`) get scoped `#pragma GCC diagnostic
  push/pop` around them.

## Running

```sh
./run.sh                           # build + full suite + valgrind
./run.sh --bonus                   # build + full suite + bounus + valgrind
./run.sh --help                    # usage
./run.sh t_d_                      # only tests whose name contains "t_d_"
```

## Platform-tolerant tests

A handful of tests accept multiple outputs because the C standard /
POSIX leaves the behavior up to the libc implementation. The runner
prints a note at the end of the summary listing them:

```
note: the following tests accept multiple outputs (libc differs across platforms):
  t_p_null      → "(nil)" (glibc) | "0x0" (BSD/macOS libc)
```

If your `ft_printf` produces one of the listed alternatives, the test
passes. Useful when you target one platform's libc but run the tests on
another.

## Adding tests

Write a `static void t_name(void)` that calls one of:

- `EXPECT_PRINTF_PURE(fmt, args...)` — compares `ft_printf` output and
  return value against libc `printf` byte-for-byte. Use for everything
  with a single canonical expected output.
- `ASSERT_FT_RET(expected_int, fmt, args...)` — asserts `ft_printf`'s
  return value equals `expected_int` *and* the number of bytes written
  also equals it. Use for libc-independent return-value anchors.
- `EXPECT_FT_ONE_OF(alts, fmt, args...)` — `alts` is a
  `NULL`-terminated `const char *const[]` of acceptable outputs. Use
  for platform-dependent cases; also list the test in the
  platform-tolerant note in `main()`.

Then add a `RUN(t_name);` line in `main()`.

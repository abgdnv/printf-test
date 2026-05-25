#!/usr/bin/env bash
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PRINTF_DIR="${PRINTF_DIR:-${SCRIPT_DIR}/..}"
BUILD_DIR="$SCRIPT_DIR/build"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--Wall -Wextra -Werror -g -O0}"

case "${1:-}" in
	--help|-h)
		cat <<EOF
usage: $0 [--clean | --help | <printf_test-args>...]

  (no args)        build libftprintf.a + printf_test, run full suite,
                   then valgrind
  --clean          remove build/ and exit
  --help, -h       this message

Anything else is forwarded to build/printf_test (after rebuilding).
Forwarded args are also passed to the valgrind run.

Examples:
  $0                  # full suite + valgrind
  $0 --clean          # wipe build/
  $0 t_d_int          # only matching tests, under valgrind too

See \`build/printf_test --help\` for printf_test's own options.
EOF
		exit 0
		;;
	--clean)
		rm -rf -- "$BUILD_DIR"
		echo "removed build/"
		exit 0
		;;
esac

if [ ! -d "$PRINTF_DIR" ]; then
	printf "Error: PRINTF_DIR=%s is not a directory\n" "$PRINTF_DIR" >&2
	exit 64
fi
if [ ! -f "$PRINTF_DIR/ft_printf.h" ] || [ ! -f "$PRINTF_DIR/ft_printf.c" ]; then
	printf "Error: no ft_printf.{c,h} in %s\n" "$PRINTF_DIR" >&2
	exit 64
fi

mkdir -p "$BUILD_DIR"
have_valgrind=0
command -v valgrind >/dev/null 2>&1 && have_valgrind=1

echo "==> Building libftprintf.a in $PRINTF_DIR"
make -C "$PRINTF_DIR" >/dev/null

if [ ! -f "$PRINTF_DIR/libftprintf.a" ]; then
	printf "Error: %s/libftprintf.a was not produced by make\n" "$PRINTF_DIR" >&2
	exit 1
fi

BIN="$BUILD_DIR/printf_test"
echo "==> Building tests"
"$CC" $CFLAGS -I"$PRINTF_DIR" \
	"$SCRIPT_DIR/tests.c" "$PRINTF_DIR/libftprintf.a" \
	-o "$BIN"

unit="?"
echo "==> Running $BIN $*"
set +e
"$BIN" "$@"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then unit="pass"; else unit="fail"; fi

vg="skip"
if [ "$have_valgrind" -eq 1 ]; then
	echo "==> Valgrind"
	set +e
	valgrind --leak-check=full --show-leak-kinds=all \
		--errors-for-leak-kinds=all --track-origins=yes \
		--trace-children=yes \
		--error-exitcode=42 --quiet "$BIN" "$@" >/dev/null 2>"$BUILD_DIR/printf_test.vg"
	rc=$?
	set -e
	# With --trace-children=yes, --error-exitcode only replaces the root
	# process's exit code. Errors found in forked children make those
	# children exit 42, which the test harness reports as FAIL — but the
	# root valgrind sees no error and returns the harness's own rc.
	# So: trust the .vg file, not rc. Valgrind output lines start with
	# `==PID==` — distinguish them from the harness's own FAIL messages
	# that also land in this file via stderr redirection.
	if grep -q "^==[0-9]\+==" "$BUILD_DIR/printf_test.vg" 2>/dev/null; then
		if grep -q "lost: [1-9]" "$BUILD_DIR/printf_test.vg"; then
			vg="leak"
		else
			vg="err"
		fi
	else
		vg="clean"
	fi
	if [ "$vg" != "clean" ]; then
		printf "    \033[33m--- valgrind diagnostics ---\033[0m\n"
		awk '
			/^==[0-9]+== (Invalid|Conditional|Use|Mismatched|Syscall|Process terminating|.* lost:)/ { p=1 }
			p { print "    " $0 }
			p && /^==[0-9]+== $/ { p=0 }
		' "$BUILD_DIR/printf_test.vg" | head -40
		printf "    \033[33m--- (full log: build/printf_test.vg) ---\033[0m\n"
	fi
	[ "$vg" = "clean" ] && rm -f "$BUILD_DIR/printf_test.vg"
fi

echo
echo "== Summary =="
printf "%-20s  %-10s  %-6s\n" "BINARY" "UNIT" "VGRIND"
printf -- "----------------------------------------\n"
color_unit=$'\033[32m'; color_vg=$'\033[32m'; reset=$'\033[0m'
exit_code=0
[ "$unit" != "pass" ] && color_unit=$'\033[31m' && exit_code=1
[ "$vg" != "clean" ] && [ "$vg" != "skip" ] && color_vg=$'\033[31m' && exit_code=1
[ "$vg" = "skip" ] && color_vg=$'\033[2m'
printf "%-20s  ${color_unit}%-10s${reset}  ${color_vg}%-6s${reset}\n" \
	"printf_test" "$unit" "$vg"

if [ "$have_valgrind" -eq 0 ]; then
	echo
	echo "(valgrind not installed — VGRIND column shows 'skip')"
fi
exit "$exit_code"

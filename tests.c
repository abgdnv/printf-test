/*
 * printf-test — single-file test runner for ft_printf.
 *
 * Strategy: for each test, call ft_printf and libc printf with the same
 * arguments, capturing each one's stdout to a temp file. Compare the
 * byte-for-byte output and the integer return value. A mismatch in either
 * fails the test.
 *
 * 42 mandatory conversions: %c %s %p %d %i %u %x %X %%
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ft_printf.h"

/* Only narrow exemptions: empty format strings and NULL %s are runtime
 * cases the C library handles gracefully. Format/argument mismatches that
 * we deliberately exercise (long passed to %d, etc.) get a localized
 * `#pragma diagnostic push/pop` at their site so the rest of the file
 * keeps full -Wformat protection. */
#pragma GCC diagnostic ignored "-Wformat-zero-length"
#pragma GCC diagnostic ignored "-Wformat-overflow"

/* ---------- Tiny assertion harness ---------- */

static int g_test_failures;
static const char *g_current_test;
#define FAIL_FMT "  \033[31mFAIL\033[0m %s:%d in %s: "

#define ASSERT_TRUE(x) do { \
	if (!(x)) { \
		fprintf(stderr, FAIL_FMT "ASSERT_TRUE(%s)\n", \
			__FILE__, __LINE__, g_current_test, #x); \
		g_test_failures++; return; \
	} } while (0)

/* ---------- Capture + compare core ---------- */

#define CAP_BUF 8192

/*
 * Redirect STDOUT_FILENO to a fresh temp file, return its fd via *out_fd.
 * Caller restores stdout (dup2) and reads the file back.
 */
static int redirect_stdout(int *saved_out)
{
	const char *dir = getenv("TMPDIR");
	char tmpl[256];
	int fd;

	if (!dir || !*dir) dir = "/tmp";
	if ((size_t)snprintf(tmpl, sizeof(tmpl), "%s/pft_XXXXXX", dir)
		>= sizeof(tmpl)) return -1;
	fd = mkstemp(tmpl);
	if (fd < 0) return -1;
	unlink(tmpl);
	fflush(stdout);
	*saved_out = dup(STDOUT_FILENO);
	if (*saved_out < 0) { close(fd); return -1; }
	if (dup2(fd, STDOUT_FILENO) < 0) {
		close(fd); close(*saved_out); return -1;
	}
	return fd;
}

static void restore_stdout(int saved)
{
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

/*
 * Returns bytes read, or:
 *   -1 on I/O error
 *   -2 if the output didn't fit in `cap` (silent truncation would mask
 *      bugs — return a distinct sentinel so the caller can fail loudly
 *      and the user knows to raise CAP_BUF).
 */
static ssize_t slurp(int fd, char *buf, size_t cap)
{
	ssize_t total = 0, n;
	char overflow_probe;

	if (lseek(fd, 0, SEEK_SET) != 0) return -1;
	while ((size_t)total < cap) {
		n = read(fd, buf + total, cap - (size_t)total);
		if (n < 0) return -1;
		if (n == 0) break;
		total += n;
	}
	if ((size_t)total == cap) {
		n = read(fd, &overflow_probe, 1);
		if (n > 0) return -2;
		if (n < 0) return -1;
	}
	return total;
}

/*
 * Format an unprintable-aware preview of `buf` into `out` (size out_cap).
 * Replaces \n with "\\n", non-printables with "\\xNN".
 */
static void preview(const char *buf, size_t n, char *out, size_t out_cap)
{
	size_t i, o = 0;
	unsigned char c;

	for (i = 0; i < n && o + 5 < out_cap; i++) {
		c = (unsigned char)buf[i];
		if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
		else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
		else if (c >= 0x20 && c < 0x7f) { out[o++] = (char)c; }
		else {
			o += (size_t)snprintf(out + o, out_cap - o, "\\x%02x", c);
		}
	}
	if (o < out_cap) out[o] = 0; else out[out_cap - 1] = 0;
}

static void report_mismatch(const char *fmt_label, int line,
	const char *ft_buf, ssize_t ft_n, int ft_ret,
	const char *sys_buf, ssize_t sys_n, int sys_ret)
{
	char ft_prev[CAP_BUF * 4 + 4];
	char sys_prev[CAP_BUF * 4 + 4];

	preview(ft_buf, (size_t)(ft_n > 0 ? ft_n : 0), ft_prev, sizeof(ft_prev));
	preview(sys_buf, (size_t)(sys_n > 0 ? sys_n : 0), sys_prev, sizeof(sys_prev));
	fprintf(stderr, FAIL_FMT "%s\n"
		"      expected (printf):    ret=%d  out=\"%s\" (len=%zd)\n"
		"      actual   (ft_printf): ret=%d  out=\"%s\" (len=%zd)\n",
		__FILE__, line, g_current_test, fmt_label,
		sys_ret, sys_prev, sys_n,
		ft_ret, ft_prev, ft_n);
}

/*
 * EXPECT_PRINTF_PURE: call ft_printf and printf with the same args; compare
 * captured stdout (byte-for-byte) and the int return value.
 *
 * NOTE: this macro evaluates `__VA_ARGS__` TWICE (once per implementation).
 * Pass only plain values, NEVER expressions with side effects.
 *   BAD:  EXPECT_PRINTF_PURE("%d", i++);
 *   BAD:  EXPECT_PRINTF_PURE("%s", strdup("x"));
 *   OK:   EXPECT_PRINTF_PURE("%d", 42);
 *
 * If captured output overflows CAP_BUF, slurp returns -2 and we abort the
 * test loudly rather than silently comparing truncated buffers.
 */
#define EXPECT_PRINTF_PURE(...) do { \
	int _saved, _tmp; \
	char _ft[CAP_BUF], _sys[CAP_BUF]; \
	ssize_t _fn, _sn; \
	int _fr, _sr; \
	\
	_tmp = redirect_stdout(&_saved); \
	ASSERT_TRUE(_tmp >= 0); \
	_fr = ft_printf(__VA_ARGS__); \
	restore_stdout(_saved); \
	_fn = slurp(_tmp, _ft, sizeof(_ft)); \
	close(_tmp); \
	if (_fn == -2) { \
		fprintf(stderr, FAIL_FMT "ft_printf output exceeded CAP_BUF (%d) — raise it\n", \
			__FILE__, __LINE__, g_current_test, CAP_BUF); \
		g_test_failures++; return; \
	} \
	ASSERT_TRUE(_fn >= 0); \
	\
	_tmp = redirect_stdout(&_saved); \
	ASSERT_TRUE(_tmp >= 0); \
	_sr = printf(__VA_ARGS__); \
	restore_stdout(_saved); \
	_sn = slurp(_tmp, _sys, sizeof(_sys)); \
	close(_tmp); \
	if (_sn == -2) { \
		fprintf(stderr, FAIL_FMT "printf output exceeded CAP_BUF (%d) — raise it\n", \
			__FILE__, __LINE__, g_current_test, CAP_BUF); \
		g_test_failures++; return; \
	} \
	ASSERT_TRUE(_sn >= 0); \
	\
	if (_fn != _sn || memcmp(_ft, _sys, (size_t)_fn) != 0 || _fr != _sr) { \
		report_mismatch(#__VA_ARGS__, __LINE__, \
			_ft, _fn, _fr, _sys, _sn, _sr); \
		g_test_failures++; return; \
	} \
} while (0)

/*
 * EXPECT_FT_ONE_OF: ft_printf output must match ONE of the strings in the
 * NULL-terminated array `alts`. Used for platform-dependent cases where
 * the C standard or POSIX leaves behavior up to the implementation
 * (e.g. `%p` with NULL → "(nil)" on glibc, "0x0" on macOS BSD libc).
 *
 * Return value must equal strlen of the matched alternative.
 */
#define EXPECT_FT_ONE_OF(alts, ...) do { \
	int _saved, _tmp; \
	char _buf[CAP_BUF]; \
	ssize_t _n; \
	int _r, _i, _ok = 0; \
	size_t _alen; \
	\
	_tmp = redirect_stdout(&_saved); \
	ASSERT_TRUE(_tmp >= 0); \
	_r = ft_printf(__VA_ARGS__); \
	restore_stdout(_saved); \
	_n = slurp(_tmp, _buf, sizeof(_buf)); \
	close(_tmp); \
	if (_n == -2) { \
		fprintf(stderr, FAIL_FMT "ft_printf output exceeded CAP_BUF (%d) — raise it\n", \
			__FILE__, __LINE__, g_current_test, CAP_BUF); \
		g_test_failures++; return; \
	} \
	if (_n < 0) { \
		fprintf(stderr, FAIL_FMT "capture failed (I/O error)\n", \
			__FILE__, __LINE__, g_current_test); \
		g_test_failures++; return; \
	} \
	for (_i = 0; (alts)[_i] != NULL; _i++) { \
		_alen = strlen((alts)[_i]); \
		if ((size_t)_n == _alen \
			&& memcmp(_buf, (alts)[_i], _alen) == 0 \
			&& _r == (int)_alen) { \
			_ok = 1; break; \
		} \
	} \
	if (!_ok) { \
		char _prev[CAP_BUF * 4 + 4]; \
		preview(_buf, (size_t)_n, _prev, sizeof(_prev)); \
		fprintf(stderr, FAIL_FMT "%s\n" \
			"      got: ret=%d out=\"%s\" (len=%zd)\n", \
			__FILE__, __LINE__, g_current_test, #__VA_ARGS__, \
			_r, _prev, _n); \
		for (_i = 0; (alts)[_i] != NULL; _i++) { \
			fprintf(stderr, "      accepted[%d]: \"%s\"\n", \
				_i, (alts)[_i]); \
		} \
		g_test_failures++; return; \
	} \
} while (0)

/* ---------- Test cases ---------- */

/* %% and plain text */

static void t_empty(void)            { EXPECT_PRINTF_PURE(""); }
static void t_plain(void)            { EXPECT_PRINTF_PURE("hello world"); }
static void t_plain_newlines(void)   { EXPECT_PRINTF_PURE("a\nb\nc\n"); }
static void t_percent_literal(void)  { EXPECT_PRINTF_PURE("100%%"); }
static void t_percent_run(void)      { EXPECT_PRINTF_PURE("%%%%%%%%"); }
static void t_percent_mixed(void)    { EXPECT_PRINTF_PURE("a%%b%%c"); }

/* %c */

static void t_c_basic(void)          { EXPECT_PRINTF_PURE("%c", 'A'); }
static void t_c_zero(void)           { EXPECT_PRINTF_PURE("[%c]", '\0'); }
static void t_c_many(void)           { EXPECT_PRINTF_PURE("%c%c%c%c", 'a', 'b', 'c', 'd'); }

/* %s */

static void t_s_basic(void)          { EXPECT_PRINTF_PURE("%s", "hello"); }
static void t_s_empty(void)          { EXPECT_PRINTF_PURE("[%s]", ""); }
static void t_s_null(void)           { EXPECT_PRINTF_PURE("%s", (char *)NULL); }
static void t_s_with_percent(void)   { EXPECT_PRINTF_PURE("%s", "%d %s %%"); }
static void t_s_multiple(void)       { EXPECT_PRINTF_PURE("%s-%s-%s", "one", "two", "three"); }
static void t_s_long(void)
{
	char big[2048];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	EXPECT_PRINTF_PURE("%s", big);
}

/* %d / %i */

static void t_d_zero(void)           { EXPECT_PRINTF_PURE("%d", 0); }
static void t_d_positive(void)       { EXPECT_PRINTF_PURE("%d", 12345); }
static void t_d_negative(void)       { EXPECT_PRINTF_PURE("%d", -42); }
static void t_d_int_max(void)        { EXPECT_PRINTF_PURE("%d", INT_MAX); }
static void t_d_int_min(void)        { EXPECT_PRINTF_PURE("%d", INT_MIN); }
static void t_i_basic(void)          { EXPECT_PRINTF_PURE("%i", -7); }
static void t_i_max(void)            { EXPECT_PRINTF_PURE("%i", INT_MAX); }
static void t_d_multiple(void)       { EXPECT_PRINTF_PURE("%d/%d/%d", 1, 2, 3); }

/* %u */

static void t_u_zero(void)           { EXPECT_PRINTF_PURE("%u", 0u); }
static void t_u_basic(void)          { EXPECT_PRINTF_PURE("%u", 12345u); }
static void t_u_max(void)            { EXPECT_PRINTF_PURE("%u", UINT_MAX); }
/* Casting -1 to unsigned int — common 42 edge case. */
static void t_u_negative_bits(void)  { EXPECT_PRINTF_PURE("%u", (unsigned int)-1); }

/* %x / %X */

static void t_x_zero(void)           { EXPECT_PRINTF_PURE("%x", 0u); }
static void t_x_basic(void)          { EXPECT_PRINTF_PURE("%x", 0xdeadbeefu); }
static void t_x_max(void)            { EXPECT_PRINTF_PURE("%x", UINT_MAX); }
static void t_X_zero(void)           { EXPECT_PRINTF_PURE("%X", 0u); }
static void t_X_basic(void)          { EXPECT_PRINTF_PURE("%X", 0xcafeBABEu); }
static void t_X_max(void)            { EXPECT_PRINTF_PURE("%X", UINT_MAX); }

/* %p */

/* `%p` with NULL is platform-dependent: glibc prints "(nil)", BSD/macOS
 * libc prints "0x0". Accept either. */
static const char *const ALTS_p_null[] = {"(nil)", "0x0", NULL};
static void t_p_null(void)           { EXPECT_FT_ONE_OF(ALTS_p_null, "%p", (void *)NULL); }
static void t_p_low(void)            { EXPECT_PRINTF_PURE("%p", (void *)0x1); }
static void t_p_value(void)          { EXPECT_PRINTF_PURE("%p", (void *)0xdeadbeefcafeULL); }

/* Mixed conversions */

static void t_mix_ds(void)           { EXPECT_PRINTF_PURE("%d %s", 42, "answer"); }
static void t_mix_all_simple(void)
{
	EXPECT_PRINTF_PURE("c=%c s=%s d=%d i=%i u=%u x=%x X=%X p=%p %%",
		'Z', "str", -1, 7, 99u, 0xabcu, 0xABCu, (void *)NULL);
}
static void t_mix_with_text(void)
{
	EXPECT_PRINTF_PURE("user=%s id=%d (status=%c) hex=0x%x\n",
		"alex", 42, 'A', 0xfeedu);
}

/* Return value cases */

static void t_ret_value_text(void)   { EXPECT_PRINTF_PURE("12345"); }
static void t_ret_value_mixed(void)  { EXPECT_PRINTF_PURE("[%d]", -100); }

/* ---------- Direct return-value assertions (no libc comparison) ----------
 * EXPECT_PRINTF_PURE checks ft_printf vs libc. These check explicit
 * invariants: e.g. ft_printf("hello") MUST return 5. Useful as a safety
 * net if libc on some platform returns something unexpected. */

#define ASSERT_FT_RET(expected_ret, ...) do { \
	int _saved, _tmp; \
	char _buf[CAP_BUF]; \
	ssize_t _n; \
	int _r; \
	\
	_tmp = redirect_stdout(&_saved); \
	ASSERT_TRUE(_tmp >= 0); \
	_r = ft_printf(__VA_ARGS__); \
	restore_stdout(_saved); \
	_n = slurp(_tmp, _buf, sizeof(_buf)); \
	close(_tmp); \
	if (_n == -2) { \
		fprintf(stderr, FAIL_FMT "ft_printf output exceeded CAP_BUF (%d) — raise it\n", \
			__FILE__, __LINE__, g_current_test, CAP_BUF); \
		g_test_failures++; return; \
	} \
	if (_n < 0) { \
		fprintf(stderr, FAIL_FMT "capture failed (I/O error)\n", \
			__FILE__, __LINE__, g_current_test); \
		g_test_failures++; return; \
	} \
	if (_r != (expected_ret)) { \
		char _prev[CAP_BUF * 4 + 4]; \
		preview(_buf, (size_t)_n, _prev, sizeof(_prev)); \
		fprintf(stderr, FAIL_FMT "expected ret=%d, got %d (out=\"%s\" len=%zd)\n", \
			__FILE__, __LINE__, g_current_test, \
			(expected_ret), _r, _prev, _n); \
		g_test_failures++; return; \
	} \
	if (_n != (expected_ret)) { \
		char _prev[CAP_BUF * 4 + 4]; \
		preview(_buf, (size_t)_n, _prev, sizeof(_prev)); \
		fprintf(stderr, FAIL_FMT \
			"ret=%d matches but bytes written=%zd (out=\"%s\")\n", \
			__FILE__, __LINE__, g_current_test, \
			_r, _n, _prev); \
		g_test_failures++; return; \
	} \
} while (0)

/* Minimal libc-independent anchors. Everything else is covered by
 * EXPECT_PRINTF_PURE comparison. */
static void t_ret_empty(void)        { ASSERT_FT_RET(0, ""); }
static void t_ret_percent(void)      { ASSERT_FT_RET(1, "%%"); }
/* Pinpoints the "ft_printf must not be strlen-based" contract: a literal
 * NUL byte counts toward the return value. */
static void t_ret_c_zero_byte(void)  { ASSERT_FT_RET(3, "%c%c%c", 'a', 0, 'b'); }

/* ---------- Additional mandatory edge cases ---------- */

/* %c: argument is read as int then truncated to unsigned char. Verifies the
 * impl uses va_arg(ap, int) and that values outside char range still print
 * the low byte. */
static void t_c_overflow_high(void)  { EXPECT_PRINTF_PURE("[%c]", '0' + 256); }
static void t_c_overflow_low(void)   { EXPECT_PRINTF_PURE("[%c]", '0' - 256); }

/* %c with a literal 0 byte in the middle of the output — the impl must not
 * stop printing at the null byte. */
static void t_c_null_then_more(void) { EXPECT_PRINTF_PURE("[%c%c%c]", '0', 0, '1'); }
static void t_c_null_with_text(void) { EXPECT_PRINTF_PURE("[%c|%c|%c]", 'a', 0, 'b'); }

/* %d / %i with a single -1 (one sign + one digit). */
static void t_d_minus_one(void)      { EXPECT_PRINTF_PURE("[%d]", -1); }

/* Wrong-sized integer args (UB per spec, but commonly tested): impl must
 * use va_arg(ap, int)/va_arg(ap, unsigned int) so the result matches libc
 * which reads the same low 32 bits. If someone wrote va_arg(ap, long),
 * these will diverge.
 *
 * The format/arg mismatch is deliberate — silence the format checker just
 * for this block. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
static void t_d_long_max(void)       { EXPECT_PRINTF_PURE("[%d]", LONG_MAX); }
static void t_d_long_min(void)       { EXPECT_PRINTF_PURE("[%d]", LONG_MIN); }
static void t_d_ulong_max(void)      { EXPECT_PRINTF_PURE("[%d]", ULONG_MAX); }
static void t_u_long_max(void)       { EXPECT_PRINTF_PURE("[%u]", LONG_MAX); }
static void t_u_long_min(void)       { EXPECT_PRINTF_PURE("[%u]", LONG_MIN); }
static void t_x_long_max(void)       { EXPECT_PRINTF_PURE("[%x]", LONG_MAX); }
static void t_X_long_min(void)       { EXPECT_PRINTF_PURE("[%X]", LONG_MIN); }
#pragma GCC diagnostic pop

/* %x/%X boundary: digit↔letter transition. */
static void t_x_9(void)              { EXPECT_PRINTF_PURE("[%x]", 9); }
static void t_x_10(void)             { EXPECT_PRINTF_PURE("[%x]", 10); }
static void t_x_15(void)             { EXPECT_PRINTF_PURE("[%x]", 15); }
static void t_x_16(void)             { EXPECT_PRINTF_PURE("[%x]", 16); }
static void t_x_17(void)             { EXPECT_PRINTF_PURE("[%x]", 17); }
static void t_X_9(void)              { EXPECT_PRINTF_PURE("[%X]", 9); }
static void t_X_10(void)             { EXPECT_PRINTF_PURE("[%X]", 10); }
static void t_X_15(void)             { EXPECT_PRINTF_PURE("[%X]", 15); }
static void t_X_16(void)             { EXPECT_PRINTF_PURE("[%X]", 16); }

/* %p boundary / extremes. */
static void t_p_minus_one(void)      { EXPECT_PRINTF_PURE("[%p]", (void *)-1L); }
static void t_p_one(void)            { EXPECT_PRINTF_PURE("[%p]", (void *)1); }
static void t_p_fifteen(void)        { EXPECT_PRINTF_PURE("[%p]", (void *)15); }
static void t_p_sixteen(void)        { EXPECT_PRINTF_PURE("[%p]", (void *)16); }
static void t_p_seventeen(void)      { EXPECT_PRINTF_PURE("[%p]", (void *)17); }

/* ---------- Bonus: flags, width, precision ---------- */

/* Plain width (no flags) */
static void t_w_d(void)              { EXPECT_PRINTF_PURE("[%5d]", 42); }
static void t_w_d_neg(void)          { EXPECT_PRINTF_PURE("[%5d]", -42); }
static void t_w_d_zero(void)         { EXPECT_PRINTF_PURE("[%5d]", 0); }
static void t_w_d_overflow(void)     { EXPECT_PRINTF_PURE("[%3d]", 12345); }
static void t_w_d_equal(void)        { EXPECT_PRINTF_PURE("[%5d]", 12345); }
static void t_w_s(void)              { EXPECT_PRINTF_PURE("[%5s]", "ab"); }
static void t_w_s_overflow(void)     { EXPECT_PRINTF_PURE("[%3s]", "abcdef"); }
static void t_w_s_empty(void)        { EXPECT_PRINTF_PURE("[%5s]", ""); }
static void t_w_c(void)              { EXPECT_PRINTF_PURE("[%5c]", 'A'); }
static void t_w_x(void)              { EXPECT_PRINTF_PURE("[%5x]", 0xab); }
static void t_w_X(void)              { EXPECT_PRINTF_PURE("[%5X]", 0xab); }
static void t_w_u(void)              { EXPECT_PRINTF_PURE("[%5u]", 42u); }
static void t_w_p(void)              { EXPECT_PRINTF_PURE("[%20p]", (void *)0x1234); }

/* `-` left align (overrides 0 when both are present) */
static void t_minus_d(void)          { EXPECT_PRINTF_PURE("[%-5d]", 42); }
static void t_minus_d_neg(void)      { EXPECT_PRINTF_PURE("[%-5d]", -42); }
static void t_minus_d_overflow(void) { EXPECT_PRINTF_PURE("[%-3d]", 12345); }
static void t_minus_s(void)          { EXPECT_PRINTF_PURE("[%-5s]", "ab"); }
static void t_minus_s_empty(void)    { EXPECT_PRINTF_PURE("[%-5s]", ""); }
static void t_minus_c(void)          { EXPECT_PRINTF_PURE("[%-5c]", 'A'); }
static void t_minus_x(void)          { EXPECT_PRINTF_PURE("[%-5x]", 0xab); }
static void t_minus_u(void)          { EXPECT_PRINTF_PURE("[%-5u]", 42u); }
/* Flag-interaction tests: standard says `-` overrides `0`, `+` overrides ` `,
 * and precision suppresses `0` for numeric. GCC warns on the redundant flag,
 * but the runtime behavior is well-defined and worth testing. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
static void t_minus_overrides_0(void){ EXPECT_PRINTF_PURE("[%-05d]", 42); }
#pragma GCC diagnostic pop
static void t_minus_p(void)          { EXPECT_PRINTF_PURE("[%-20p]", (void *)0x1234); }

/* `0` zero-pad (numeric only) */
static void t_zero_d(void)           { EXPECT_PRINTF_PURE("[%05d]", 42); }
static void t_zero_d_neg(void)       { EXPECT_PRINTF_PURE("[%05d]", -42); }
static void t_zero_d_zero(void)      { EXPECT_PRINTF_PURE("[%05d]", 0); }
static void t_zero_d_intmin(void)    { EXPECT_PRINTF_PURE("[%015d]", INT_MIN); }
static void t_zero_d_intmax(void)    { EXPECT_PRINTF_PURE("[%015d]", INT_MAX); }
static void t_zero_x(void)           { EXPECT_PRINTF_PURE("[%05x]", 0xab); }
static void t_zero_X(void)           { EXPECT_PRINTF_PURE("[%05X]", 0xab); }
static void t_zero_u(void)           { EXPECT_PRINTF_PURE("[%05u]", 42u); }
static void t_zero_i(void)           { EXPECT_PRINTF_PURE("[%05i]", -7); }

/* `.` precision */
static void t_prec_d(void)           { EXPECT_PRINTF_PURE("[%.5d]", 42); }
static void t_prec_d_neg(void)       { EXPECT_PRINTF_PURE("[%.5d]", -42); }
static void t_prec_d_zero_zero(void) { EXPECT_PRINTF_PURE("[%.0d]", 0); }
static void t_prec_d_zero_val(void)  { EXPECT_PRINTF_PURE("[%.0d]", 5); }
static void t_prec_d_intmax(void)    { EXPECT_PRINTF_PURE("[%.15d]", INT_MAX); }
static void t_prec_d_intmin(void)    { EXPECT_PRINTF_PURE("[%.15d]", INT_MIN); }
static void t_prec_d_dot_only(void)  { EXPECT_PRINTF_PURE("[%.d]", 5); }
static void t_prec_d_dot_zero(void)  { EXPECT_PRINTF_PURE("[%.d]", 0); }
static void t_prec_s(void)           { EXPECT_PRINTF_PURE("[%.3s]", "hello"); }
static void t_prec_s_zero(void)      { EXPECT_PRINTF_PURE("[%.0s]", "hello"); }
static void t_prec_s_huge(void)      { EXPECT_PRINTF_PURE("[%.10s]", "hi"); }
static void t_prec_s_dot_only(void)  { EXPECT_PRINTF_PURE("[%.s]", "hello"); }
static void t_prec_s_exact(void)     { EXPECT_PRINTF_PURE("[%.5s]", "hello"); }
static void t_prec_x(void)           { EXPECT_PRINTF_PURE("[%.5x]", 0xab); }
static void t_prec_x_zero(void)      { EXPECT_PRINTF_PURE("[%.0x]", 0); }
static void t_prec_X(void)           { EXPECT_PRINTF_PURE("[%.5X]", 0xab); }
static void t_prec_u(void)           { EXPECT_PRINTF_PURE("[%.5u]", 42u); }
static void t_prec_with_width(void)  { EXPECT_PRINTF_PURE("[%10.5d]", 42); }
static void t_prec_with_width_l(void){ EXPECT_PRINTF_PURE("[%-10.5d]", 42); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
static void t_prec_overrides_0(void) { EXPECT_PRINTF_PURE("[%05.3d]", 42); }
#pragma GCC diagnostic pop
static void t_prec_width_s(void)     { EXPECT_PRINTF_PURE("[%10.3s]", "hello"); }
static void t_prec_width_s_l(void)   { EXPECT_PRINTF_PURE("[%-10.3s]", "hello"); }

/* `#` alternate form (x / X). For 0 value, # has no effect. */
static void t_sharp_x(void)          { EXPECT_PRINTF_PURE("[%#x]", 0xab); }
static void t_sharp_x_zero(void)     { EXPECT_PRINTF_PURE("[%#x]", 0); }
static void t_sharp_X(void)          { EXPECT_PRINTF_PURE("[%#X]", 0xab); }
static void t_sharp_X_zero(void)     { EXPECT_PRINTF_PURE("[%#X]", 0); }
static void t_sharp_x_width(void)    { EXPECT_PRINTF_PURE("[%#10x]", 0xab); }
static void t_sharp_x_zero_pad(void) { EXPECT_PRINTF_PURE("[%#08x]", 0xab); }
static void t_sharp_x_left(void)     { EXPECT_PRINTF_PURE("[%-#10x]", 0xab); }
static void t_sharp_x_prec(void)     { EXPECT_PRINTF_PURE("[%#.5x]", 0xab); }

/* ` ` (space) flag — signed numeric: leading space for non-negative */
static void t_space_d(void)          { EXPECT_PRINTF_PURE("[% d]", 42); }
static void t_space_d_neg(void)      { EXPECT_PRINTF_PURE("[% d]", -42); }
static void t_space_d_zero(void)     { EXPECT_PRINTF_PURE("[% d]", 0); }
static void t_space_d_width(void)    { EXPECT_PRINTF_PURE("[% 5d]", 42); }
static void t_space_d_prec(void)     { EXPECT_PRINTF_PURE("[% .5d]", 42); }
static void t_space_i(void)          { EXPECT_PRINTF_PURE("[% i]", 7); }

/* `+` plus flag — overrides space */
static void t_plus_d(void)           { EXPECT_PRINTF_PURE("[%+d]", 42); }
static void t_plus_d_neg(void)       { EXPECT_PRINTF_PURE("[%+d]", -42); }
static void t_plus_d_zero(void)      { EXPECT_PRINTF_PURE("[%+d]", 0); }
static void t_plus_d_intmax(void)    { EXPECT_PRINTF_PURE("[%+d]", INT_MAX); }
static void t_plus_d_intmin(void)    { EXPECT_PRINTF_PURE("[%+d]", INT_MIN); }
static void t_plus_width(void)       { EXPECT_PRINTF_PURE("[%+5d]", 42); }
static void t_plus_zero_pad(void)    { EXPECT_PRINTF_PURE("[%+05d]", 42); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
static void t_plus_overrides_space(void) { EXPECT_PRINTF_PURE("[% +d]", 42); }
#pragma GCC diagnostic pop
static void t_plus_left(void)        { EXPECT_PRINTF_PURE("[%-+5d]", -7); }

/* Stress: big width / precision (catches impls with fixed-size internal
 * buffers, e.g. a 256-byte scratch buf — fairly common 42 bug). */
static void t_big_width_d(void)      { EXPECT_PRINTF_PURE("[%500d]", 42); }
static void t_big_width_left(void)   { EXPECT_PRINTF_PURE("[%-500d]", 42); }
static void t_big_width_s(void)      { EXPECT_PRINTF_PURE("[%500s]", "x"); }
static void t_big_prec_d(void)       { EXPECT_PRINTF_PURE("[%.500d]", 42); }
static void t_big_prec_d_neg(void)   { EXPECT_PRINTF_PURE("[%.500d]", -42); }
static void t_big_prec_s(void)
{
	char big[1500];
	memset(big, 'y', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	EXPECT_PRINTF_PURE("[%.500s]", big);
}

/* Many consecutive conversions of the same type — stresses va_list
 * traversal. Mixed-type chains are covered by t_mix_all_simple. */
static void t_many_d(void)
{
	EXPECT_PRINTF_PURE("%d %d %d %d %d %d %d %d %d %d",
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}

/* Big combinations / sanity */
static void t_combo_neg_width_prec(void)  { EXPECT_PRINTF_PURE("[%10.5d]", -42); }
static void t_combo_left_prec(void)       { EXPECT_PRINTF_PURE("[%-10.5d]", -42); }
static void t_combo_zero_intmin(void)     { EXPECT_PRINTF_PURE("[%020d]", INT_MIN); }
static void t_combo_two_format(void)
{
	EXPECT_PRINTF_PURE("[%-10s|%10s|%5d|%-5d]", "hi", "hi", 7, 7);
}
static void t_combo_sharp_left(void)      { EXPECT_PRINTF_PURE("[%-#10x]", 0xab); }

/* ---------- Runner: fork+timeout per test ---------- */

#ifndef PFT_TIMEOUT_SEC
# define PFT_TIMEOUT_SEC 5
#endif

static const char *g_filter;
static int g_bonus;
static int g_outer_pass, g_outer_fail, g_outer_timeout, g_outer_crash, g_outer_skipped;
static int g_last_signal;

enum { TR_PASS = 0, TR_FAIL = 1, TR_TIMEOUT = 2, TR_CRASH = 3 };

/* No-op SIGALRM handler. Without it, the default action is to terminate
 * the whole harness when alarm() fires. We just want EINTR to break the
 * blocking waitpid below. */
static void sigalrm_noop(int sig) { (void)sig; }

static int run_forked(void (*test)(void))
{
	pid_t pid, r;
	int status;

	/* Flush parent buffers before forking. Otherwise the child inherits
	 * the parent's pending stdout content (PASS lines accumulated since
	 * last flush) and `redirect_stdout` in the child fflush()es them to
	 * the real terminal — producing exponentially-growing duplicate
	 * output. */
	fflush(stdout);
	fflush(stderr);

	g_last_signal = 0;
	pid = fork();
	if (pid < 0) { perror("fork"); return TR_CRASH; }
	if (pid == 0)
	{
		g_test_failures = 0;
		test();
		_exit(g_test_failures == 0 ? 0 : 1);
	}
	alarm(PFT_TIMEOUT_SEC);
	r = waitpid(pid, &status, 0);
	alarm(0);
	if (r < 0)
	{
		if (errno == EINTR)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return TR_TIMEOUT;
		}
		perror("waitpid");
		return TR_CRASH;
	}
	if (WIFSIGNALED(status))
	{
		g_last_signal = WTERMSIG(status);
		return TR_CRASH;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status) == 0 ? TR_PASS : TR_FAIL;
	return TR_CRASH;
}

/* RUN: drives one test through the configured runner, prints a result
 * line for every outcome (PASS, FAIL, TIMEOUT, CRASH), honours the
 * argv-based filter. The trick `do { ... if (cond) break; ... } while(0)`
 * lets us early-exit from the macro body cleanly. */
#define RUN(t) do { \
	int _r; \
	if (g_filter && strstr(#t, g_filter) == NULL) { \
		g_outer_skipped++; \
		break; \
	} \
	g_current_test = #t; \
	_r = run_forked(t); \
	if (_r == TR_PASS) { \
		g_outer_pass++; \
		printf("  \033[32mPASS\033[0m    %s\n", #t); \
	} else if (_r == TR_TIMEOUT) { \
		g_outer_timeout++; \
		printf("  \033[31mTIMEOUT\033[0m %s (>%ds)\n", #t, PFT_TIMEOUT_SEC); \
	} else if (_r == TR_CRASH) { \
		g_outer_crash++; \
		if (g_last_signal) \
			printf("  \033[31mCRASH\033[0m   %s (signal %d %s)\n", \
				#t, g_last_signal, strsignal(g_last_signal)); \
		else \
			printf("  \033[31mCRASH\033[0m   %s\n", #t); \
	} else { \
		g_outer_fail++; \
		printf("  \033[31mFAIL\033[0m    %s\n", #t); \
	} \
} while (0)

/* Self-test bodies. These run inside run_forked from harness_self_test. */
static void st_forced_fail(void) { ASSERT_TRUE(0); }
static void st_forced_pass(void) { /* no-op, asserts nothing */ }

/*
 * Sanity-check the test infrastructure itself before running any real
 * tests. Validates:
 *   1. stdout capture works (probe write → slurp returns identical bytes)
 *   2. truncation detection fires when output exceeds CAP_BUF
 *   3. fork-path end-to-end: a deliberately failing test under run_forked
 *      reports TR_FAIL; a no-op test reports TR_PASS.
 *
 * Exits non-zero if any check fails.
 */
static void harness_self_test_or_die(void)
{
	int saved, tmp, devnull, saved_err, r;
	char buf[64];
	char tiny[16];
	int i;
	ssize_t n;

	/* 1) capture round-trip */
	tmp = redirect_stdout(&saved);
	if (tmp < 0) { fprintf(stderr, "FATAL: redirect_stdout failed\n"); exit(2); }
	printf("probe-%d", 42);
	restore_stdout(saved);
	n = slurp(tmp, buf, sizeof(buf));
	close(tmp);
	if (n != 8 || memcmp(buf, "probe-42", 8) != 0) {
		fprintf(stderr, "FATAL: capture broken (n=%zd buf=\"%.*s\")\n",
			n, (int)(n > 0 ? n : 0), buf);
		exit(2);
	}

	/* 2) truncation detection */
	tmp = redirect_stdout(&saved);
	if (tmp < 0) { fprintf(stderr, "FATAL: redirect_stdout failed\n"); exit(2); }
	for (i = 0; i < 100; i++) printf("xxxxxxxxxx");
	restore_stdout(saved);
	n = slurp(tmp, tiny, sizeof(tiny));
	close(tmp);
	if (n != -2) {
		fprintf(stderr, "FATAL: truncation undetected (n=%zd)\n", n);
		exit(2);
	}

	/* 3) fork-path end-to-end. Silence stderr so the child's expected
	 * FAIL message doesn't leak into the user's terminal. */
	devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0) { fprintf(stderr, "FATAL: open /dev/null\n"); exit(2); }
	fflush(stderr);
	saved_err = dup(STDERR_FILENO);
	if (saved_err < 0 || dup2(devnull, STDERR_FILENO) < 0) {
		fprintf(stderr, "FATAL: cannot redirect stderr for self-test\n");
		exit(2);
	}
	close(devnull);

	g_current_test = "<self-test:forced_fail>";
	r = run_forked(st_forced_fail);
	if (r != TR_FAIL) {
		fflush(stderr); dup2(saved_err, STDERR_FILENO); close(saved_err);
		fprintf(stderr, "FATAL: fork-path failure detection broken (got %d, expected %d)\n",
			r, TR_FAIL);
		exit(2);
	}

	g_current_test = "<self-test:forced_pass>";
	r = run_forked(st_forced_pass);
	if (r != TR_PASS) {
		fflush(stderr); dup2(saved_err, STDERR_FILENO); close(saved_err);
		fprintf(stderr, "FATAL: fork-path success detection broken (got %d, expected %d)\n",
			r, TR_PASS);
		exit(2);
	}

	fflush(stderr);
	dup2(saved_err, STDERR_FILENO);
	close(saved_err);
}

static void install_sigalrm_handler(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigalrm_noop;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;  /* No SA_RESTART — we want waitpid to return EINTR. */
	if (sigaction(SIGALRM, &sa, NULL) < 0) {
		fprintf(stderr, "FATAL: sigaction(SIGALRM) failed\n");
		exit(2);
	}
}

static void print_usage(const char *progname)
{
	printf(
		"usage: %s [<test-substring> | --bonus | --help]\n"
		"\n"
		"  <test-substring>   run only tests whose name contains this substring\n"
		"  --bonus            also run the bonus tests (width/precision/flags)\n"
		"  -h, --help         this message\n"
		"\n"
		"environment:\n"
		"  TMPDIR=<dir>       directory for capture temp files (default /tmp)\n",
		progname);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		}
		if (strcmp(argv[1], "--bonus") == 0)
			g_bonus = 1;
		else
			g_filter = argv[1];
	}

	install_sigalrm_handler();
	harness_self_test_or_die();
	printf("== harness self-test OK ==\n");
	printf("== ft_printf tests (timeout=%ds%s%s) ==\n",
		PFT_TIMEOUT_SEC,
		g_filter ? ", filter=" : "",
		g_filter ? g_filter : "");

	RUN(t_empty);
	RUN(t_plain);
	RUN(t_plain_newlines);
	RUN(t_percent_literal);
	RUN(t_percent_run);
	RUN(t_percent_mixed);

	RUN(t_c_basic);
	RUN(t_c_zero);
	RUN(t_c_many);

	RUN(t_s_basic);
	RUN(t_s_empty);
	RUN(t_s_null);
	RUN(t_s_with_percent);
	RUN(t_s_multiple);
	RUN(t_s_long);

	RUN(t_d_zero);
	RUN(t_d_positive);
	RUN(t_d_negative);
	RUN(t_d_int_max);
	RUN(t_d_int_min);
	RUN(t_i_basic);
	RUN(t_i_max);
	RUN(t_d_multiple);

	RUN(t_u_zero);
	RUN(t_u_basic);
	RUN(t_u_max);
	RUN(t_u_negative_bits);

	RUN(t_x_zero);
	RUN(t_x_basic);
	RUN(t_x_max);
	RUN(t_X_zero);
	RUN(t_X_basic);
	RUN(t_X_max);

	RUN(t_p_null);
	RUN(t_p_low);
	RUN(t_p_value);

	RUN(t_mix_ds);
	RUN(t_mix_all_simple);
	RUN(t_mix_with_text);

	RUN(t_ret_value_text);
	RUN(t_ret_value_mixed);

	/* --- Direct return-value anchors (no libc comparison) --- */
	RUN(t_ret_empty);
	RUN(t_ret_percent);
	RUN(t_ret_c_zero_byte);

	/* --- Additional mandatory --- */
	RUN(t_c_overflow_high);
	RUN(t_c_overflow_low);
	RUN(t_c_null_then_more);
	RUN(t_c_null_with_text);
	RUN(t_d_minus_one);
	RUN(t_d_long_max);
	RUN(t_d_long_min);
	RUN(t_d_ulong_max);
	RUN(t_u_long_max);
	RUN(t_u_long_min);
	RUN(t_x_long_max);
	RUN(t_X_long_min);
	RUN(t_x_9);
	RUN(t_x_10);
	RUN(t_x_15);
	RUN(t_x_16);
	RUN(t_x_17);
	RUN(t_X_9);
	RUN(t_X_10);
	RUN(t_X_15);
	RUN(t_X_16);
	RUN(t_p_minus_one);
	RUN(t_p_one);
	RUN(t_p_fifteen);
	RUN(t_p_sixteen);
	RUN(t_p_seventeen);
	if (g_bonus) {
		/* --- Bonus: width --- */
		RUN(t_w_d);
		RUN(t_w_d_neg);
		RUN(t_w_d_zero);
		RUN(t_w_d_overflow);
		RUN(t_w_d_equal);
		RUN(t_w_s);
		RUN(t_w_s_overflow);
		RUN(t_w_s_empty);
		RUN(t_w_c);
		RUN(t_w_x);
		RUN(t_w_X);
		RUN(t_w_u);
		RUN(t_w_p);

		/* --- Bonus: `-` --- */
		RUN(t_minus_d);
		RUN(t_minus_d_neg);
		RUN(t_minus_d_overflow);
		RUN(t_minus_s);
		RUN(t_minus_s_empty);
		RUN(t_minus_c);
		RUN(t_minus_x);
		RUN(t_minus_u);
		RUN(t_minus_overrides_0);
		RUN(t_minus_p);

		/* --- Bonus: `0` --- */
		RUN(t_zero_d);
		RUN(t_zero_d_neg);
		RUN(t_zero_d_zero);
		RUN(t_zero_d_intmin);
		RUN(t_zero_d_intmax);
		RUN(t_zero_x);
		RUN(t_zero_X);
		RUN(t_zero_u);
		RUN(t_zero_i);

		/* --- Bonus: `.` --- */
		RUN(t_prec_d);
		RUN(t_prec_d_neg);
		RUN(t_prec_d_zero_zero);
		RUN(t_prec_d_zero_val);
		RUN(t_prec_d_intmax);
		RUN(t_prec_d_intmin);
		RUN(t_prec_d_dot_only);
		RUN(t_prec_d_dot_zero);
		RUN(t_prec_s);
		RUN(t_prec_s_zero);
		RUN(t_prec_s_huge);
		RUN(t_prec_s_dot_only);
		RUN(t_prec_s_exact);
		RUN(t_prec_x);
		RUN(t_prec_x_zero);
		RUN(t_prec_X);
		RUN(t_prec_u);
		RUN(t_prec_with_width);
		RUN(t_prec_with_width_l);
		RUN(t_prec_overrides_0);
		RUN(t_prec_width_s);
		RUN(t_prec_width_s_l);

		/* --- Bonus: `#` --- */
		RUN(t_sharp_x);
		RUN(t_sharp_x_zero);
		RUN(t_sharp_X);
		RUN(t_sharp_X_zero);
		RUN(t_sharp_x_width);
		RUN(t_sharp_x_zero_pad);
		RUN(t_sharp_x_left);
		RUN(t_sharp_x_prec);

		/* --- Bonus: ` ` (space) --- */
		RUN(t_space_d);
		RUN(t_space_d_neg);
		RUN(t_space_d_zero);
		RUN(t_space_d_width);
		RUN(t_space_d_prec);
		RUN(t_space_i);

		/* --- Bonus: `+` --- */
		RUN(t_plus_d);
		RUN(t_plus_d_neg);
		RUN(t_plus_d_zero);
		RUN(t_plus_d_intmax);
		RUN(t_plus_d_intmin);
		RUN(t_plus_width);
		RUN(t_plus_zero_pad);
		RUN(t_plus_overrides_space);
		RUN(t_plus_left);

		/* --- Stress: big width / precision / many args --- */
		RUN(t_big_width_d);
		RUN(t_big_width_left);
		RUN(t_big_width_s);
		RUN(t_big_prec_d);
		RUN(t_big_prec_d_neg);
		RUN(t_big_prec_s);
		RUN(t_many_d);

		/* --- Bonus: combos --- */
		RUN(t_combo_neg_width_prec);
		RUN(t_combo_left_prec);
		RUN(t_combo_zero_intmin);
		RUN(t_combo_two_format);
		RUN(t_combo_sharp_left);
	}
	/* A filter that matched nothing is almost always a typo. Refusing
	 * loudly is better than a confident "0 pass, 0 fail" green. */
	if (g_filter
		&& (g_outer_pass + g_outer_fail + g_outer_timeout + g_outer_crash) == 0)
	{
		fprintf(stderr, "error: filter \"%s\" matched no tests\n", g_filter);
		return 2;
	}

	printf("== %d pass, %d fail, %d timeout, %d crash",
		g_outer_pass, g_outer_fail, g_outer_timeout, g_outer_crash);
	if (g_outer_skipped) printf(", %d skipped", g_outer_skipped);
	printf(" ==\n");

	/* Platform-tolerant tests use EXPECT_FT_ONE_OF instead of comparing
	 * against the local libc. Mention only those that were not filtered
	 * out — otherwise the note advertises tests the user never ran. */
	if (!g_filter || strstr("t_p_null", g_filter)) {
		printf("note: platform-tolerant tests (libc differs across platforms):\n");
		printf("  t_p_null      → \"(nil)\" (glibc) | \"0x0\" (BSD/macOS libc)\n");
	}

	return (g_outer_fail + g_outer_timeout + g_outer_crash) == 0 ? 0 : 1;
}

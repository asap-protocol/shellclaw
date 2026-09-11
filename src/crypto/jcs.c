/**
 * @file jcs.c
 * @brief RFC 8785 JCS canonicalization for cJSON trees (manifest signing subset).
 */
#include "crypto/jcs.h"

#include "cJSON.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JCS_INIT_CAP 256U

typedef struct {
	unsigned char *buf;
	size_t len;
	size_t cap;
} jcs_buf_t;

static int jcs_buf_reserve(jcs_buf_t *b, size_t need)
{
	size_t new_cap;
	unsigned char *p;
	if (!b)
		return -1;
	if (need <= b->cap)
		return 0;
	new_cap = b->cap ? b->cap : JCS_INIT_CAP;
	while (new_cap < need) {
		if (new_cap > (SIZE_MAX / 2U))
			return -1;
		new_cap *= 2U;
	}
	p = (unsigned char *)realloc(b->buf, new_cap);
	if (!p)
		return -1;
	b->buf = p;
	b->cap = new_cap;
	return 0;
}

static int jcs_buf_append(jcs_buf_t *b, const char *s, size_t n)
{
	if (!b || !s)
		return -1;
	if (jcs_buf_reserve(b, b->len + n + 1U) != 0)
		return -1;
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
	return 0;
}

static int jcs_buf_append_cstr(jcs_buf_t *b, const char *s)
{
	if (!s)
		return -1;
	return jcs_buf_append(b, s, strlen(s));
}

static int jcs_buf_append_byte(jcs_buf_t *b, char c)
{
	return jcs_buf_append(b, &c, 1U);
}

static int jcs_key_cmp(const void *a, const void *b)
{
	const char *ka = *(const char *const *)a;
	const char *kb = *(const char *const *)b;
	return strcmp(ka, kb);
}

static int jcs_append_escaped_string(jcs_buf_t *b, const char *s)
{
	const unsigned char *p;

	if (!s)
		return -1;
	if (jcs_buf_append_byte(b, '"') != 0)
		return -1;
	for (p = (const unsigned char *)s; *p != '\0'; p++) {
		unsigned char c = *p;
		if (c == '"') {
			if (jcs_buf_append_cstr(b, "\\\"") != 0)
				return -1;
		} else if (c == '\\') {
			if (jcs_buf_append_cstr(b, "\\\\") != 0)
				return -1;
		} else if (c == '\b') {
			if (jcs_buf_append_cstr(b, "\\b") != 0)
				return -1;
		} else if (c == '\f') {
			if (jcs_buf_append_cstr(b, "\\f") != 0)
				return -1;
		} else if (c == '\n') {
			if (jcs_buf_append_cstr(b, "\\n") != 0)
				return -1;
		} else if (c == '\r') {
			if (jcs_buf_append_cstr(b, "\\r") != 0)
				return -1;
		} else if (c == '\t') {
			if (jcs_buf_append_cstr(b, "\\t") != 0)
				return -1;
		} else if (c < 0x20U) {
			char hex[7];
			snprintf(hex, sizeof(hex), "\\u%04x", (unsigned)c);
			if (jcs_buf_append_cstr(b, hex) != 0)
				return -1;
		} else {
			if (jcs_buf_append_byte(b, (char)c) != 0)
				return -1;
		}
	}
	return jcs_buf_append_byte(b, '"');
}

static int jcs_is_safe_integer(double v, long long *out)
{
	long long n;
	if (!isfinite(v))
		return 0;
	if (v < -9007199254740991.0 || v > 9007199254740991.0)
		return 0;
	n = (long long)v;
	if ((double)n != v)
		return 0;
	*out = n;
	return 1;
}

/* Find the shortest "%.Ng" rendering of v that strtod parses back to a double
 * bit-identical to v. Bit comparison (not ==) avoids NaN pitfalls and matches
 * the reference jcs 0.2.1 shortest round-trip behavior. Returns the precision
 * used (1..17) and fills buf, or -1 on failure. */
static int jcs_shortest_roundtrip(double v, char *buf, size_t buf_size)
{
	uint64_t target_bits;
	int prec;

	memcpy(&target_bits, &v, sizeof(target_bits));
	for (prec = 1; prec <= 17; prec++) {
		char trial[64];
		double parsed;
		uint64_t parsed_bits;

		snprintf(trial, sizeof(trial), "%.*g", prec, v);
		parsed = strtod(trial, NULL);
		memcpy(&parsed_bits, &parsed, sizeof(parsed_bits));
		if (parsed_bits == target_bits) {
			if (strlen(trial) + 1U > buf_size)
				return -1;
			memcpy(buf, trial, strlen(trial) + 1U);
			return prec;
		}
	}
	/* Fall back to full precision; it always round-trips for finite doubles. */
	snprintf(buf, buf_size, "%.17g", v);
	return 17;
}

/* Append the ES Number.prototype.toString exponential form: d[.ddd]e[+|-]exp.
 * s is the k significant digits (no sign, no point); exp is the signed base-10
 * exponent of the leading digit (n - 1). Caller has already stripped the sign. */
static int jcs_append_exponential(jcs_buf_t *b, const char *s, int k, int exp)
{
	char num[16];
	int nlen;

	if (jcs_buf_append_byte(b, s[0]) != 0)
		return -1;
	if (k > 1) {
		int i;
		if (jcs_buf_append_byte(b, '.') != 0)
			return -1;
		for (i = 1; i < k; i++)
			if (jcs_buf_append_byte(b, s[i]) != 0)
				return -1;
	}
	if (jcs_buf_append_byte(b, 'e') != 0)
		return -1;
	if (exp < 0) {
		if (jcs_buf_append_byte(b, '-') != 0)
			return -1;
		nlen = snprintf(num, sizeof(num), "%d", -exp);
	} else {
		if (jcs_buf_append_byte(b, '+') != 0)
			return -1;
		nlen = snprintf(num, sizeof(num), "%d", exp);
	}
	if (nlen < 0 || (size_t)nlen >= sizeof(num))
		return -1;
	return jcs_buf_append_cstr(b, num);
}

/* Append the ES Number.prototype.toString decimal form from significant digits s
 * (length k, no sign, no point) and decimal-point position n (1-indexed from the
 * left of s): n <= 0 -> "0." + (-n) zeros + s; 0 < n <= 21 -> insert point at n;
 * n > 21 is handled by the caller via the exponential path. */
static int jcs_append_decimal(jcs_buf_t *b, const char *s, int k, int n)
{
	int i;

	if (n <= 0) {
		if (jcs_buf_append_cstr(b, "0.") != 0)
			return -1;
		for (i = n; i < 0; i++)
			if (jcs_buf_append_byte(b, '0') != 0)
				return -1;
		return jcs_buf_append_cstr(b, s);
	}
	/* 0 < n <= 21 (and n <= k, since the caller only reaches here when n <= 21
	 * and the value is in decimal range; if n > k we pad with zeros below). */
	for (i = 0; i < k || i < n; i++) {
		if (i == n) {
			if (jcs_buf_append_byte(b, '.') != 0)
				return -1;
		}
		if (i < k) {
			if (jcs_buf_append_byte(b, s[i]) != 0)
				return -1;
		} else {
			if (jcs_buf_append_byte(b, '0') != 0)
				return -1;
		}
	}
	return 0;
}

static int jcs_append_number(jcs_buf_t *b, double v)
{
	long long n;
	char tmp[64];
	const char *p;
	char sign;
	char mant[64];
	char digits[64];
	int k;
	int e;
	int n_pos;

	if (!isfinite(v))
		return -1;
	if (jcs_is_safe_integer(v, &n)) {
		snprintf(tmp, sizeof(tmp), "%lld", n);
		return jcs_buf_append_cstr(b, tmp);
	}
	if (jcs_shortest_roundtrip(v, tmp, sizeof(tmp)) < 0)
		return -1;

	/* "%g" may already have produced a plain decimal (no exponent); those are
	 * always in the ES decimal range (-5 <= e <= 16), so keep them verbatim. */
	p = strchr(tmp, 'e');
	if (p == NULL)
		p = strchr(tmp, 'E');
	if (p == NULL)
		return jcs_buf_append_cstr(b, tmp);

	/* Parse "<sign?><mantissa>e<sign?><exp>" from the shortest %g rendering. */
	sign = '\0';
	{
		const char *mp = tmp;
		size_t mlen;
		if (*mp == '-' || *mp == '+')
			sign = *mp++;
		mlen = (size_t)(p - mp);
		if (mlen >= sizeof(mant))
			return -1;
		memcpy(mant, mp, mlen);
		mant[mlen] = '\0';
	}
	e = (int)strtol(p + 1, NULL, 10);

	/* Collapse the mantissa into significant digits (drop the '.') and count k. */
	{
		size_t di = 0;
		const char *mp;
		for (mp = mant; *mp != '\0'; mp++) {
			if (*mp == '.')
				continue;
			if (di >= sizeof(digits) - 1U)
				return -1;
			digits[di++] = *mp;
		}
		digits[di] = '\0';
		k = (int)di;
	}
	if (k == 0)
		return -1;

	/* n = decimal-point position (1-indexed from the left of the normalized
	 * mantissa); ES uses n <= -6 or n > 21 to select exponential form. */
	n_pos = e + 1;

	if (sign == '-' && jcs_buf_append_byte(b, '-') != 0)
		return -1;

	if (n_pos > 21)
		return jcs_append_exponential(b, digits, k, n_pos - 1);
	if (n_pos <= -6)
		return jcs_append_exponential(b, digits, k, n_pos - 1);
	return jcs_append_decimal(b, digits, k, n_pos);
}

static int jcs_serialize(const cJSON *node, jcs_buf_t *b);

static int jcs_serialize_object(const cJSON *obj, jcs_buf_t *b)
{
	const cJSON *child;
	const char **keys;
	int count;
	int i;
	int first;

	if (!obj || !b)
		return -1;
	count = 0;
	for (child = obj->child; child != NULL; child = child->next)
		count++;
	if (count == 0)
		return jcs_buf_append_cstr(b, "{}");
	keys = (const char **)calloc((size_t)count, sizeof(const char *));
	if (!keys)
		return -1;
	i = 0;
	for (child = obj->child; child != NULL; child = child->next) {
		if (!child->string) {
			free(keys);
			return -1;
		}
		keys[i++] = child->string;
	}
	qsort(keys, (size_t)count, sizeof(const char *), jcs_key_cmp);
	if (jcs_buf_append_byte(b, '{') != 0) {
		free(keys);
		return -1;
	}
	first = 1;
	for (i = 0; i < count; i++) {
		child = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
		if (!child) {
			free(keys);
			return -1;
		}
		if (!first) {
			if (jcs_buf_append_byte(b, ',') != 0) {
				free(keys);
				return -1;
			}
		}
		first = 0;
		if (jcs_append_escaped_string(b, keys[i]) != 0) {
			free(keys);
			return -1;
		}
		if (jcs_buf_append_byte(b, ':') != 0) {
			free(keys);
			return -1;
		}
		if (jcs_serialize(child, b) != 0) {
			free(keys);
			return -1;
		}
	}
	free(keys);
	return jcs_buf_append_byte(b, '}');
}

static int jcs_serialize_array(const cJSON *arr, jcs_buf_t *b)
{
	const cJSON *child;
	int first;

	if (!arr || !b)
		return -1;
	if (jcs_buf_append_byte(b, '[') != 0)
		return -1;
	first = 1;
	for (child = arr->child; child != NULL; child = child->next) {
		if (!first) {
			if (jcs_buf_append_byte(b, ',') != 0)
				return -1;
		}
		first = 0;
		if (jcs_serialize(child, b) != 0)
			return -1;
	}
	return jcs_buf_append_byte(b, ']');
}

static int jcs_serialize(const cJSON *node, jcs_buf_t *b)
{
	if (!node || !b)
		return -1;
	if (cJSON_IsNull(node))
		return jcs_buf_append_cstr(b, "null");
	if (cJSON_IsFalse(node))
		return jcs_buf_append_cstr(b, "false");
	if (cJSON_IsTrue(node))
		return jcs_buf_append_cstr(b, "true");
	if (cJSON_IsNumber(node))
		return jcs_append_number(b, node->valuedouble);
	if (cJSON_IsString(node))
		return jcs_append_escaped_string(b, node->valuestring);
	if (cJSON_IsArray(node))
		return jcs_serialize_array(node, b);
	if (cJSON_IsObject(node))
		return jcs_serialize_object(node, b);
	return -1;
}

int jcs_canonicalize(const cJSON *root, unsigned char **out, size_t *out_len)
{
	jcs_buf_t b;

	if (!root || !out || !out_len)
		return -1;
	memset(&b, 0, sizeof(b));
	if (jcs_serialize(root, &b) != 0) {
		free(b.buf);
		return -1;
	}
	*out = b.buf;
	*out_len = b.len;
	return 0;
}

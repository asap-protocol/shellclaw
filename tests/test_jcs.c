/**
 * @file test_jcs.c
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "crypto/jcs.h"
#include "cJSON.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_jcs_canonicalize_sorted_keys(void)
{
	cJSON *root;
	unsigned char *out;
	size_t out_len;
	const char *expect = "{\"a\":1,\"z\":2}";

	root = cJSON_Parse("{\"z\":2,\"a\":1}");
	ASSERT(root != NULL);
	ASSERT(jcs_canonicalize(root, &out, &out_len) == 0);
	ASSERT(out_len == strlen(expect));
	ASSERT(memcmp(out, expect, out_len) == 0);
	free(out);
	cJSON_Delete(root);
	return 0;
}

static int test_jcs_escapes_primitives_and_arrays(void)
{
	cJSON *root;
	unsigned char *out;
	size_t out_len;
	const char *expect =
	    "{\"arr\":[null,true,false,42,1.5,\"a\\tb\\nc\"],\"empty\":{}}";

	root = cJSON_Parse(
	    "{\"empty\":{},\"arr\":[null,true,false,42,1.5,\"a\\tb\\nc\"]}");
	ASSERT(root != NULL);
	ASSERT(jcs_canonicalize(root, &out, &out_len) == 0);
	ASSERT(out_len == strlen(expect));
	ASSERT(memcmp(out, expect, out_len) == 0);
	free(out);
	cJSON_Delete(root);
	return 0;
}

static int test_jcs_rejects_invalid_inputs(void)
{
	cJSON *nan_node;
	unsigned char *out = NULL;
	size_t out_len = 0;

	ASSERT(jcs_canonicalize(NULL, &out, &out_len) == -1);
	nan_node = cJSON_CreateNumber(NAN);
	ASSERT(nan_node != NULL);
	ASSERT(jcs_canonicalize(nan_node, &out, &out_len) == -1);
	cJSON_Delete(nan_node);
	return 0;
}

static int test_jcs_rejects_non_finite_number(void)
{
	cJSON *root;
	unsigned char *out = NULL;
	size_t out_len = 0;

	root = cJSON_CreateNumber(INFINITY);
	ASSERT(root != NULL);
	ASSERT(jcs_canonicalize(root, &out, &out_len) == -1);
	cJSON_Delete(root);
	return 0;
}

static int test_jcs_string_escape_coverage(void)
{
	cJSON *root;
	unsigned char *out;
	size_t out_len;
	const char *expect =
	    "{\"ctl\":\"\\u0001\",\"q\":\"\\\"\\\\\\b\\f\\r\\t\"}";

	root = cJSON_Parse("{\"q\":\"\\\"\\\\\\b\\f\\r\\t\",\"ctl\":\"\\u0001\"}");
	ASSERT(root != NULL);
	ASSERT(jcs_canonicalize(root, &out, &out_len) == 0);
	ASSERT(out_len == strlen(expect));
	ASSERT(memcmp(out, expect, out_len) == 0);
	free(out);
	cJSON_Delete(root);
	return 0;
}

/* Byte-exact RFC 8785 number serialization, verified against the reference
 * `jcs` 0.2.1 package (the one asap.crypto.signing.canonicalize uses). Each
 * entry pins one input double to the exact bytes the reference emits, so a
 * regression in jcs_append_number (e.g. %.17g, missing + in exponent, or the
 * 1e20 -> 100000000000000000000 rewrite) fails the build. */
static int test_jcs_number_table(void)
{
	static const struct {
		const char *name;
		double value;
		const char *expect;
	} cases[] = {
		{ "0.1", 0.1, "{\"n\":0.1}" },
		{ "1e20", 1e20, "{\"n\":100000000000000000000}" },
		{ "1e21", 1e21, "{\"n\":1e+21}" },
		{ "1e22", 1e22, "{\"n\":1e+22}" },
		{ "1e23", 1e23, "{\"n\":1e+23}" },
		{ "1e-6", 1e-6, "{\"n\":0.000001}" },
		{ "1e-7", 1e-7, "{\"n\":1e-7}" },
		{ "1.5e-10", 1.5e-10, "{\"n\":1.5e-10}" },
		{ "100.0", 100.0, "{\"n\":100}" },
		{ "1.0", 1.0, "{\"n\":1}" },
		{ "0.5", 0.5, "{\"n\":0.5}" },
		{ "2.2", 2.2, "{\"n\":2.2}" },
		{ "300.0", 300.0, "{\"n\":300}" },
		{ "2^53-1", 9007199254740991.0, "{\"n\":9007199254740991}" },
		{ "1e16", 1e16, "{\"n\":10000000000000000}" },
		{ "1e17", 1e17, "{\"n\":100000000000000000}" },
		{ "1.234e18", 1234567890123456789.0,
			"{\"n\":1234567890123456800}" },
		{ "pi", 3.141592653589793, "{\"n\":3.141592653589793}" },
		{ "max_double", 1.7976931348623157e308,
			"{\"n\":1.7976931348623157e+308}" },
		{ "0.0", 0.0, "{\"n\":0}" },
		{ "-0.0", -0.0, "{\"n\":0}" },
		{ "-1.5", -1.5, "{\"n\":-1.5}" },
		{ "2^53", 9007199254740992.0, "{\"n\":9007199254740992}" },
		{ "1e-8", 1e-8, "{\"n\":1e-8}" },
		{ "1.5e-7", 1.5e-7, "{\"n\":1.5e-7}" },
		{ "100000.0", 100000.0, "{\"n\":100000}" },
		{ "0.0001", 0.0001, "{\"n\":0.0001}" },
		{ "0.00001", 0.00001, "{\"n\":0.00001}" },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		cJSON *root;
		cJSON *n;
		unsigned char *out = NULL;
		size_t out_len = 0;
		size_t expect_len = strlen(cases[i].expect);

		root = cJSON_CreateObject();
		ASSERT(root != NULL);
		n = cJSON_CreateNumber(cases[i].value);
		ASSERT(n != NULL);
		cJSON_AddItemToObject(root, "n", n);
		ASSERT(jcs_canonicalize(root, &out, &out_len) == 0);
		if (out_len != expect_len ||
		    memcmp(out, cases[i].expect, expect_len) != 0) {
			fprintf(stderr,
				"FAIL: jcs number %s: got \"%.*s\" want \"%s\"\n",
				cases[i].name, (int)out_len,
				out ? (char *)out : "(null)", cases[i].expect);
			free(out);
			cJSON_Delete(root);
			return 1;
		}
		free(out);
		cJSON_Delete(root);
	}
	return 0;
}

static int test_jcs_multibyte_utf8_passthrough(void)
{
	cJSON *root;
	unsigned char *out;
	size_t out_len;
	const char *expect = "{\"s\":\"ação\"}";

	/* RFC 8785 passes non-ASCII UTF-8 bytes through unchanged (only < 0x20 and
	 * the JSON structural chars are escaped). "ação" is UTF-8: a c3 a7 c3 a3 o. */
	root = cJSON_CreateObject();
	ASSERT(root != NULL);
	{
		cJSON *s = cJSON_CreateString("ação");
		ASSERT(s != NULL);
		cJSON_AddItemToObject(root, "s", s);
	}
	ASSERT(jcs_canonicalize(root, &out, &out_len) == 0);
	ASSERT(out_len == strlen(expect));
	ASSERT(memcmp(out, expect, out_len) == 0);
	free(out);
	cJSON_Delete(root);
	return 0;
}

int main(int argc, char **argv)
{
	int failed = 0;
	(void)argc;
	(void)argv;
	if (test_jcs_canonicalize_sorted_keys() != 0) {
		fprintf(stderr, "test_jcs_canonicalize_sorted_keys failed\n");
		failed++;
	}
	if (test_jcs_escapes_primitives_and_arrays() != 0) {
		fprintf(stderr, "test_jcs_escapes_primitives_and_arrays failed\n");
		failed++;
	}
	if (test_jcs_rejects_invalid_inputs() != 0) {
		fprintf(stderr, "test_jcs_rejects_invalid_inputs failed\n");
		failed++;
	}
	if (test_jcs_rejects_non_finite_number() != 0) {
		fprintf(stderr, "test_jcs_rejects_non_finite_number failed\n");
		failed++;
	}
	if (test_jcs_string_escape_coverage() != 0) {
		fprintf(stderr, "test_jcs_string_escape_coverage failed\n");
		failed++;
	}
	if (test_jcs_number_table() != 0) {
		fprintf(stderr, "test_jcs_number_table failed\n");
		failed++;
	}
	if (test_jcs_multibyte_utf8_passthrough() != 0) {
		fprintf(stderr, "test_jcs_multibyte_utf8_passthrough failed\n");
		failed++;
	}
	if (failed == 0)
		printf("test_jcs: all tests passed\n");
	return failed;
}

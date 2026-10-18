/**
 * @file test_provider.c
 * @brief Verify provider_t vtable and stub provider compile and run.
 */

#include "providers/provider.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int test_stub_provider_vtable(void)
{
	const provider_t *p = provider_stub_get();
	ASSERT(p != NULL);
	ASSERT(p->name != NULL);
	ASSERT(p->init != NULL);
	ASSERT(p->chat != NULL);
	ASSERT(p->cleanup != NULL);
	ASSERT(p->init(NULL) == 0);
	provider_response_t response = {0};
	ASSERT(p->chat(NULL, 0, NULL, 0, &response) == 0);
	p->cleanup();
	provider_response_clear(&response);
	return 0;
}

static int test_provider_set_error_null_response(void)
{
	provider_set_error(NULL, "should not crash");
	return 0;
}

static int test_model_id_for_backend(void)
{
	const char *example = "anthropic/claude-sonnet-4-20250514";
	const char *bare_claude = "claude-sonnet-4-5";
	const char *openai_prefixed = "openai/gpt-4o";
	const char *bare_gpt = "gpt-4o";
	const char *local_name = "Phi-3-mini";
	const char *empty = "";
	const char *stripped;

	stripped = provider_model_id_for_backend(example, "anthropic");
	ASSERT(stripped == example + strlen("anthropic/"));
	ASSERT(strcmp(stripped, "claude-sonnet-4-20250514") == 0);
	ASSERT(provider_model_id_for_backend(example, "openai") == NULL);
	ASSERT(provider_model_id_for_backend(bare_claude, "openai") == NULL);
	ASSERT(provider_model_id_for_backend(bare_claude, "anthropic") == bare_claude);
	stripped = provider_model_id_for_backend(openai_prefixed, "openai");
	ASSERT(stripped == openai_prefixed + strlen("openai/"));
	ASSERT(strcmp(stripped, "gpt-4o") == 0);
	ASSERT(provider_model_id_for_backend(openai_prefixed, "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend(bare_gpt, "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend(bare_gpt, "openai") == bare_gpt);
	ASSERT(provider_model_id_for_backend("chatgpt-4o-latest", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend("o1-preview", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend("o3-mini", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend("o4-mini", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend(local_name, "anthropic") == local_name);
	ASSERT(provider_model_id_for_backend(local_name, "openai") == local_name);
	ASSERT(provider_model_id_for_backend(empty, "anthropic") == empty);
	ASSERT(provider_model_id_for_backend(NULL, "openai") == NULL);
	ASSERT(provider_error_allows_fallback_retry(
		"OpenAI skipped: agent model is not an OpenAI id") == 1);
	ASSERT(provider_error_allows_fallback_retry("OpenAI API HTTP 400") == 0);
	return 0;
}

static int test_model_id_for_backend_edge_cases(void)
{
	const char *multi_slash = "openrouter/anthropic/claude-3";
	const char *other_vendor = "meta-llama/llama-3-70b";
	const char *stripped;

	/* Multi-slash and unknown vendors belong to no backend: skip (NULL) so
	 * the fallback chain moves on instead of a 4xx that stops it. */
	ASSERT(provider_model_id_for_backend(multi_slash, "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend(multi_slash, "openai") == NULL);
	ASSERT(provider_model_id_for_backend(other_vendor, "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend(other_vendor, "openai") == NULL);
	/* Empty vendor or id segment: the raw id would 400; skip instead. */
	ASSERT(provider_model_id_for_backend("anthropic/", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend("openai/", "openai") == NULL);
	ASSERT(provider_model_id_for_backend("/foo", "anthropic") == NULL);
	ASSERT(provider_model_id_for_backend("/foo", "openai") == NULL);
	/* Vendor tag matching is case-insensitive like the provider router. */
	stripped = provider_model_id_for_backend("Anthropic/claude-x", "anthropic");
	ASSERT(stripped != NULL && strcmp(stripped, "claude-x") == 0);
	ASSERT(provider_model_id_for_backend("Anthropic/claude-x", "openai") == NULL);
	stripped = provider_model_id_for_backend("OpenAI/gpt-4o", "openai");
	ASSERT(stripped != NULL && strcmp(stripped, "gpt-4o") == 0);
	return 0;
}

int main(void)
{
	if (test_stub_provider_vtable() != 0) return 1;
	if (test_provider_set_error_null_response() != 0) return 1;
	if (test_model_id_for_backend() != 0) return 1;
	if (test_model_id_for_backend_edge_cases() != 0) return 1;
	printf("test_provider: all tests passed\n");
	return 0;
}

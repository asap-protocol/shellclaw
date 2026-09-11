/**
 * @file uri_match.h
 * @brief Shared URI path matching for the gateway HTTP layer.
 */
#ifndef SHELLCLAW_GATEWAY_URI_MATCH_H
#define SHELLCLAW_GATEWAY_URI_MATCH_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Exact URI path match (length + bytes). */
static inline int uri_exact_eq(const char *uri, int uri_len, const char *path)
{
	size_t plen = strlen(path);
	return (uri_len == (int)plen && strncmp(uri, path, plen) == 0);
}

/** Prefix match: @p uri starts with @p prefix (length may exceed prefix). */
static inline int uri_has_prefix(const char *uri, int uri_len, const char *prefix)
{
	size_t plen = strlen(prefix);
	return (uri_len >= (int)plen && strncmp(uri, prefix, plen) == 0);
}

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_URI_MATCH_H */

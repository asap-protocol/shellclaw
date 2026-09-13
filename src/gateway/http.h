/**
 * @file http.h
 * @brief HTTP server and REST API (libwebsockets for HTTP+WebSocket same port).
 */

#ifndef SHELLCLAW_GATEWAY_HTTP_H
#define SHELLCLAW_GATEWAY_HTTP_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;
struct auth_ctx;

static inline const char *http_host_without_brackets(const char *host, char *buf,
						       size_t buf_sz)
{
	size_t n;

	if (!host || host[0] != '[')
		return host;
	n = strlen(host);
	if (n < 2 || host[n - 1] != ']')
		return host;
	n -= 2;
	if (n >= buf_sz)
		return host;
	memcpy(buf, host + 1, n);
	buf[n] = '\0';
	return buf;
}

/**
 * Non-zero if @p host is all-interfaces: 0.0.0.0, *, IPv6 any, or empty.
 * Those require allow_bind_all; otherwise http_start must fail closed.
 *
 * Example: http_host_is_bind_all("::") != 0; http_host_is_bind_all("::1") == 0.
 */
static inline int http_host_is_bind_all(const char *host)
{
	char unbrack[INET6_ADDRSTRLEN];
	const char *p;
	struct in_addr a4;
	struct in6_addr a6;

	if (!host || host[0] == '\0' || strcmp(host, "*") == 0)
		return 1;
	p = http_host_without_brackets(host, unbrack, sizeof(unbrack));
	if (p[0] == '\0' || strcmp(p, "*") == 0)
		return 1;
	if (inet_pton(AF_INET, p, &a4) == 1)
		return a4.s_addr == htonl(INADDR_ANY);
	if (inet_pton(AF_INET6, p, &a6) == 1)
		return IN6_IS_ADDR_UNSPECIFIED(&a6);
	return 0;
}

/**
 * LWS iface for @p host. NULL iface is INADDR_ANY (all NICs).
 *
 * @param host            config_gateway_host value (may be empty).
 * @param allow_bind_all  config_gateway_allow_bind_all.
 * @param iface_out       Set to NULL (bind all) or @p host. Must be non-NULL.
 * @return 0 on success, -1 if bind-all is requested without allow_bind_all.
 *
 * Example: http_listen_iface("0.0.0.0", 1, &iface) == 0 && iface == NULL.
 */
static inline int http_listen_iface(const char *host, int allow_bind_all,
				     const char **iface_out)
{
	if (!iface_out)
		return -1;
	if (http_host_is_bind_all(host)) {
		if (!allow_bind_all)
			return -1;
		*iface_out = NULL;
		return 0;
	}
	*iface_out = host;
	return 0;
}

/**
 * Start HTTP+WebSocket server on config host:port.
 * Binds the listen socket to config_gateway_host (default 127.0.0.1 via
 * info.iface). Rejects bind-all hosts unless allow_bind_all is true.
 *
 * @param cfg         Configuration (host, port, allow_bind_all).
 * @param auth_ctx    Auth context for token validation.
 * @param config_path Path to config.toml for PUT /api/config.
 * @return 0 on success, non-zero on error.
 */
int http_start(const config_t *cfg, struct auth_ctx *auth_ctx, const char *config_path);

/**
 * Stop HTTP server. Safe to call if not started.
 */
void http_stop(void);

/** Push provider_status WebSocket JSON (same payload as GET /api/status plus `type`). */
void http_emit_ws_provider_status(void);

/**
 * After SIGHUP config reload: swap the server's live config pointer (no HTTP rebind).
 * No-op when the gateway was not started.
 */
void http_set_live_config(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_HTTP_H */

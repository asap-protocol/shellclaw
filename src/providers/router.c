/**
 * @file router.c
 * @brief Composite provider: `fallback_chain`, transport/5xx retry, 4xx stop, snapshots for `/api/status`, periodic recovery.
 */

#include "core/config.h"
#include "providers/provider.h"
#include "cJSON.h"
#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROUTER_BACKEND_MAX 32
#define SNAP_ACT_SZ 96
#define SNAP_ERR_SZ 768
#define ROUTER_RECOVERY_INTERVAL_DEFAULT_SEC 300U
#define ROUTER_PROBE_TIMEOUT_SEC 5L
#define ROUTER_PROBE_CONNECT_SEC 2L
#define ANTHROPIC_MODELS_URL "https://api.anthropic.com/v1/models"
#define ANTHROPIC_VERSION_HDR "anthropic-version: 2023-06-01"

static pthread_mutex_t g_router_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_snap_act[SNAP_ACT_SZ];
static char g_snap_err[SNAP_ERR_SZ];
static size_t g_snap_chain_ix;

static const provider_t *g_backends[ROUTER_BACKEND_MAX];
static size_t g_backend_count;
static const config_t *g_router_cfg;

static unsigned g_recovery_interval_sec = ROUTER_RECOVERY_INTERVAL_DEFAULT_SEC;
static time_t g_recovery_last_wall;
static provider_router_status_changed_fn g_status_changed_cb;

static int str_case_equal(const char *a, const char *b)
{
	if (!a || !b) return 0;
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
}

static const provider_t *lookup_backend(const char *name)
{
	if (!name || !name[0]) return NULL;
	if (str_case_equal(name, "stub")) return provider_stub_get();
	if (str_case_equal(name, "stub-b")) return provider_stub_b_get();
	if (str_case_equal(name, "anthropic")) return provider_anthropic_get();
	if (str_case_equal(name, "openai")) return provider_openai_get();
	if (str_case_equal(name, "local")) return provider_local_get();
	return NULL;
}

static int backends_contains(const provider_t *const *list, size_t n, const provider_t *p)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (list[i] == p) return 1;
	return 0;
}

static void router_snap_clear_all(void)
{
	pthread_mutex_lock(&g_router_snap_mu);
	g_snap_act[0] = '\0';
	g_snap_err[0] = '\0';
	g_snap_chain_ix = 0U;
	pthread_mutex_unlock(&g_router_snap_mu);
}

void provider_router_set_status_changed_callback(provider_router_status_changed_fn fn)
{
	g_status_changed_cb = fn;
}

static void router_snap_set_active(const char *name, size_t chain_ix)
{
	char new_act[SNAP_ACT_SZ];
	size_t old_ix;
	char old_act[SNAP_ACT_SZ];
	int changed;
	provider_router_status_changed_fn notify = NULL;
	if (!name || !name[0]) {
		new_act[0] = '\0';
	} else {
		strncpy(new_act, name, SNAP_ACT_SZ - 1U);
		new_act[SNAP_ACT_SZ - 1U] = '\0';
	}
	pthread_mutex_lock(&g_router_snap_mu);
	old_ix = g_snap_chain_ix;
	strncpy(old_act, g_snap_act, SNAP_ACT_SZ - 1U);
	old_act[SNAP_ACT_SZ - 1U] = '\0';
	changed = (old_ix != chain_ix) || (strcmp(old_act, new_act) != 0);
	g_snap_chain_ix = chain_ix;
	memcpy(g_snap_act, new_act, SNAP_ACT_SZ);
	if (changed)
		notify = g_status_changed_cb;
	pthread_mutex_unlock(&g_router_snap_mu);
	if (notify)
		notify();
}

static void snapshot_set_last_err(const char *msg)
{
	pthread_mutex_lock(&g_router_snap_mu);
	if (!msg || !msg[0]) {
		g_snap_err[0] = '\0';
	} else {
		strncpy(g_snap_err, msg, SNAP_ERR_SZ - 1);
		g_snap_err[SNAP_ERR_SZ - 1] = '\0';
	}
	pthread_mutex_unlock(&g_router_snap_mu);
}

static size_t router_snap_read_chain_ix(void)
{
	size_t ix;
	pthread_mutex_lock(&g_router_snap_mu);
	ix = g_snap_chain_ix;
	pthread_mutex_unlock(&g_router_snap_mu);
	return ix;
}

void provider_router_active_backend_snapshot(char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0U) return;
	pthread_mutex_lock(&g_router_snap_mu);
	strncpy(dst, g_snap_act, dst_sz - 1U);
	dst[dst_sz - 1U] = '\0';
	pthread_mutex_unlock(&g_router_snap_mu);
}

void provider_router_last_error_snapshot(char *dst, size_t dst_sz)
{
	if (!dst || dst_sz == 0U) return;
	pthread_mutex_lock(&g_router_snap_mu);
	strncpy(dst, g_snap_err, dst_sz - 1U);
	dst[dst_sz - 1U] = '\0';
	pthread_mutex_unlock(&g_router_snap_mu);
}

static void clear_backends_list(void)
{
	memset(g_backends, 0, sizeof(g_backends));
	g_backend_count = 0U;
	g_router_cfg = NULL;
}

static size_t router_discard_probe_write(const char *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr;
	(void)userdata;
	if (nmemb != 0 && size > 1048576U / nmemb) return 0;
	return size * nmemb;
}

static int router_curl_get_discard(const char *url, struct curl_slist *headers)
{
	CURL *curl;
	CURLcode res;
	if (!url || !url[0]) return 0;
	curl = curl_easy_init();
	if (!curl) return 0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, ROUTER_PROBE_TIMEOUT_SEC);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, ROUTER_PROBE_CONNECT_SEC);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, router_discard_probe_write);
	if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res == CURLE_OK ? 1 : 0;
}

static int router_build_openai_models_url(const char *endpoint, char *buf, size_t bufsz)
{
	const char *fallback_ep = "https://api.openai.com/v1/chat/completions";
	const char *ep = (endpoint && endpoint[0]) ? endpoint : fallback_ep;
	const char *v1 = strstr(ep, "/v1/");
	size_t prefix;
	if (!v1) return -1;
	prefix = (size_t)(v1 - ep) + 3U;
	if (prefix + 8U >= bufsz) return -1;
	memcpy(buf, ep, prefix);
	buf[prefix] = '\0';
	strncat(buf, "/models", bufsz - prefix - 1U);
	return 0;
}

static int router_probe_backend(const provider_t *p, const config_t *cfg)
{
	if (!p || !cfg) return 0;
	if (p == provider_stub_get() || p == provider_stub_b_get())
		return 1;
	if (p == provider_local_get()) {
		const char *ep = config_provider_local_endpoint(cfg);
		int ok = ep && ep[0] && provider_local_endpoint_reachable(ep);
		if (ok) provider_local_recovery_probe_succeeded();
		return ok;
	}
	if (p == provider_openai_get()) {
		char url[512];
		const char *envn = config_provider_openai_api_key_env(cfg);
		const char *key = (envn && envn[0]) ? getenv(envn) : NULL;
		struct curl_slist *hdr = NULL;
		size_t blen;
		char *auth;
		int ok;
		if (!key || !key[0]) return 0;
		if (router_build_openai_models_url(config_provider_openai_endpoint(cfg), url, sizeof(url)) != 0)
			return 0;
		blen = strlen(key) + 32U;
		auth = malloc(blen);
		if (!auth) return 0;
		snprintf(auth, blen, "Authorization: Bearer %s", key);
		hdr = curl_slist_append(hdr, auth);
		free(auth);
		ok = hdr ? router_curl_get_discard(url, hdr) : 0;
		if (hdr) curl_slist_free_all(hdr);
		return ok;
	}
	if (p == provider_anthropic_get()) {
		const char *envn = config_provider_anthropic_api_key_env(cfg);
		const char *key = (envn && envn[0]) ? getenv(envn) : NULL;
		struct curl_slist *hdr = NULL;
		size_t blen;
		char *keyhdr;
		int ok;
		if (!key || !key[0]) return 0;
		blen = strlen(key) + 32U;
		keyhdr = malloc(blen);
		if (!keyhdr) return 0;
		snprintf(keyhdr, blen, "x-api-key: %s", key);
		hdr = curl_slist_append(hdr, ANTHROPIC_VERSION_HDR);
		hdr = curl_slist_append(hdr, keyhdr);
		free(keyhdr);
		ok = hdr ? router_curl_get_discard(ANTHROPIC_MODELS_URL, hdr) : 0;
		if (hdr) curl_slist_free_all(hdr);
		return ok;
	}
	return 0;
}

void provider_router_periodic_recovery_set_interval_seconds(unsigned interval_sec)
{
	g_recovery_interval_sec = interval_sec ? interval_sec : ROUTER_RECOVERY_INTERVAL_DEFAULT_SEC;
}

void provider_router_periodic_recovery_reset_timer(void)
{
	g_recovery_last_wall = (time_t)0;
}

void provider_router_periodic_recovery_tick(time_t now_wall)
{
	size_t active_ix;
	size_t j;
	if (!g_router_cfg || g_backend_count < 2U)
		return;
	active_ix = router_snap_read_chain_ix();
	if (active_ix == 0U)
		return;
	if (g_recovery_last_wall != (time_t)0 &&
	    (unsigned long)(now_wall - g_recovery_last_wall) < (unsigned long)g_recovery_interval_sec)
		return;
	g_recovery_last_wall = now_wall;
	for (j = 0U; j < active_ix && j < g_backend_count; j++) {
		if (router_probe_backend(g_backends[j], g_router_cfg)) {
			const char *nm = g_backends[j]->name ? g_backends[j]->name : "";
			fprintf(stderr, "shellclaw: fallback recovery: primary \"%s\" reachable — switching back\n", nm);
			router_snap_set_active(nm, j);
			snapshot_set_last_err("");
			return;
		}
	}
}

/** Walk `fallback_chain`; init each backend at most once; duplicate pointers skipped. Returns -1 if none usable. */
static int fallback_composite_init(const config_t *cfg)
{
	int i;
	int chain_n;
	clear_backends_list();
	router_snap_clear_all();
	if (!cfg) return -1;
	g_router_cfg = cfg;
	chain_n = config_provider_fallback_chain_count(cfg);
	if (chain_n <= 0) return -1;
	for (i = 0; i < chain_n; i++) {
		const provider_t *p;
		const char *nm = config_provider_fallback_chain_entry(cfg, i);
		if (!nm || !nm[0]) continue;
		p = lookup_backend(nm);
		if (!p) {
			fprintf(stderr, "shellclaw: fallback_chain unknown provider \"%s\" (skipped)\n", nm);
			continue;
		}
		if (backends_contains(g_backends, g_backend_count, p))
			continue;
		if (p->init(cfg) != 0)
			continue;
		if (g_backend_count >= ROUTER_BACKEND_MAX)
			break;
		g_backends[g_backend_count++] = p;
	}
	if (g_backend_count == 0U) {
		clear_backends_list();
		return -1;
	}
	router_snap_set_active(g_backends[0]->name ? g_backends[0]->name : "", 0U);
	if (g_backends[0] == provider_local_get() && provider_local_is_unavailable())
		snapshot_set_last_err("Local inference server unreachable");
	g_recovery_last_wall = time(NULL);
	return 0;
}

/** Reverse cleanup matches forward init semantics for global singleton backends. */
static void fallback_composite_cleanup(void)
{
	size_t bid;
	pthread_mutex_lock(&g_router_snap_mu);
	g_snap_act[0] = '\0';
	g_snap_err[0] = '\0';
	g_snap_chain_ix = 0U;
	pthread_mutex_unlock(&g_router_snap_mu);
	for (bid = g_backend_count; bid > 0U; bid--)
		g_backends[bid - 1U]->cleanup();
	clear_backends_list();
	g_recovery_last_wall = (time_t)0;
}

/** Try backends in chain order until success or terminal client failure. */
static int fallback_composite_chat(const provider_message_t *messages, size_t message_count,
                                  const provider_tool_def_t *tools, size_t tool_count,
                                  provider_response_t *response)
{
	size_t ix;
	if (!g_router_cfg || g_backend_count == 0U) {
		if (response) provider_set_error(response, "Fallback router has no backends");
		return -1;
	}
	provider_response_clear(response);
	for (ix = 0U; ix < g_backend_count; ix++) {
		int ret = g_backends[ix]->chat(messages, message_count, tools, tool_count, response);
		if (ret == 0) {
			if (ix > 0U)
				fprintf(stderr, "shellclaw: fallback: using backend \"%s\"\n",
					g_backends[ix]->name ? g_backends[ix]->name : "?");
			router_snap_set_active(g_backends[ix]->name ? g_backends[ix]->name : "", ix);
			snapshot_set_last_err("");
			return 0;
		}
		snapshot_set_last_err(response && response->content ? response->content : "");
		if (ix + 1U == g_backend_count ||
		    !provider_error_allows_fallback_retry(response ? response->content : NULL))
			return ret;
		provider_response_clear(response);
	}
	return -1;
}

char *provider_router_api_status_json(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *arr;
	char act[SNAP_ACT_SZ];
	char err[SNAP_ERR_SZ];
	size_t aix;
	char *out;
	if (!root) return NULL;
	provider_router_active_backend_snapshot(act, sizeof(act));
	provider_router_last_error_snapshot(err, sizeof(err));
	aix = router_snap_read_chain_ix();
	cJSON_AddItemToObject(root, "active_provider", cJSON_CreateString(act));
	cJSON_AddItemToObject(root, "generated_at", cJSON_CreateNumber((double)time(NULL)));
	if (err[0]) cJSON_AddItemToObject(root, "last_error", cJSON_CreateString(err));
	arr = cJSON_CreateArray();
	if (!arr) {
		cJSON_Delete(root);
		return NULL;
	}
	if (g_router_cfg && g_backend_count > 0U) {
		for (size_t i = 0U; i < g_backend_count; i++) {
			cJSON *pobj = cJSON_CreateObject();
			const char *nm = g_backends[i]->name ? g_backends[i]->name : "";
			const char *role;
			if (!pobj) {
				cJSON_Delete(arr);
				cJSON_Delete(root);
				return NULL;
			}
			cJSON_AddItemToObject(pobj, "name", cJSON_CreateString(nm));
			if (i < aix) {
				cJSON_AddItemToObject(pobj, "reachable", cJSON_CreateFalse());
				role = "unavailable";
			} else if (i == aix) {
				cJSON_AddItemToObject(pobj, "reachable", cJSON_CreateTrue());
				role = (i == 0U) ? "primary" : "fallback";
			} else {
				cJSON_AddItemToObject(pobj, "reachable", cJSON_CreateTrue());
				role = "standby";
			}
			cJSON_AddItemToObject(pobj, "role", cJSON_CreateString(role));
			cJSON_AddItemToArray(arr, pobj);
		}
	}
	cJSON_AddItemToObject(root, "providers", arr);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static const provider_t fallback_router_provider = {
	.name = "shellclaw-router",
	.init = fallback_composite_init,
	.chat = fallback_composite_chat,
	.cleanup = fallback_composite_cleanup,
};

const provider_t *provider_router_get(const config_t *cfg)
{
	if (!cfg) return NULL;
	return &fallback_router_provider;
}

void provider_router_set_live_config(const config_t *cfg)
{
	if (!cfg || g_backend_count == 0U)
		return;
	g_router_cfg = cfg;
}

/**
 * @file config.c
 * @brief Configuration loader: TOML parse + environment overrides.
 */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "toml.h"

#define ERRBUF_COPY(buf, sz, msg) do { \
	if ((buf) && (sz) > 0) { \
		size_t n = strlen(msg); \
		if (n >= (sz)) n = (sz) - 1; \
		memcpy((buf), (msg), n); \
		(buf)[n] = '\0'; \
	} \
} while (0)

#define DEFAULT_MAX_TOOL_ITERATIONS 20
#define DEFAULT_MAX_CONTEXT_MESSAGES 40
#define DEFAULT_MAX_TOKENS 4096
#define DEFAULT_TEMPERATURE 0.7
#define DEFAULT_SHELL_TIMEOUT_SEC 60
#define DEFAULT_GATEWAY_PORT 18789

#define ENV_AGENT_MODEL          "SHELLCLAW_AGENT_MODEL"
#define ENV_AGENT_MAX_TOKENS     "SHELLCLAW_AGENT_MAX_TOKENS"
#define ENV_AGENT_TEMPERATURE    "SHELLCLAW_AGENT_TEMPERATURE"
#define ENV_AGENT_MAX_TOOL_ITER  "SHELLCLAW_AGENT_MAX_TOOL_ITERATIONS"
#define ENV_AGENT_MAX_CTX_MSG    "SHELLCLAW_AGENT_MAX_CONTEXT_MESSAGES"
#define ENV_MEMORY_DB_PATH       "SHELLCLAW_MEMORY_DB_PATH"
#define ENV_SKILLS_DIR            "SHELLCLAW_SKILLS_DIR"
#define ENV_OPENAI_ENDPOINT      "SHELLCLAW_OPENAI_ENDPOINT"
#define ENV_DEFAULT_PROVIDER     "SHELLCLAW_DEFAULT_PROVIDER"
#define ENV_GATEWAY_ENABLED      "SHELLCLAW_GATEWAY_ENABLED"
#define ENV_GATEWAY_HOST         "SHELLCLAW_GATEWAY_HOST"
#define ENV_GATEWAY_PORT         "SHELLCLAW_GATEWAY_PORT"
#define ENV_GATEWAY_ALLOW_BIND   "SHELLCLAW_GATEWAY_ALLOW_BIND_ALL"
#define ENV_ASAP_REGISTRY_URL         "SHELLCLAW_ASAP_REGISTRY_URL"
#define ENV_ASAP_REVOCATION_LIST_URL  "SHELLCLAW_ASAP_REVOCATION_LIST_URL"
#define ENV_ASAP_DESCRIPTION          "SHELLCLAW_ASAP_DESCRIPTION"
#define ENV_ASAP_PUBLIC_BASE_URL      "SHELLCLAW_ASAP_PUBLIC_BASE_URL"
#define ENV_HARDWARE_CLASS            "SHELLCLAW_HARDWARE_CLASS"
#define ENV_HARDWARE_MODEL            "SHELLCLAW_HARDWARE_MODEL"
#define DEFAULT_ASAP_DESCRIPTION \
	"C-native edge-AI ASAP agent for ShellClaw edge hardware."
#define DEFAULT_ASAP_PUBLIC_BASE_URL "https://shellclaw.example.com"
#define MAX_HARDWARE_IO_ENTRIES 16
#define MAX_ASAP_SKILL_DESCRIPTIONS 64
#define ENV_FALLBACK_CHAIN           "SHELLCLAW_FALLBACK_CHAIN"
#define ENV_LOCAL_ENDPOINT           "SHELLCLAW_LOCAL_ENDPOINT"
#define ENV_LOCAL_MODEL              "SHELLCLAW_LOCAL_MODEL"
#define ENV_AGENT_LATITUDE           "SHELLCLAW_AGENT_LATITUDE"
#define ENV_AGENT_LONGITUDE          "SHELLCLAW_AGENT_LONGITUDE"
#define ENV_AGENT_COUNTRY_CODE       "SHELLCLAW_AGENT_COUNTRY_CODE"
#define ENV_HARDWARE_BOARD           "SHELLCLAW_BOARD"
#define ENV_HARDWARE_I2C_BUS         "SHELLCLAW_I2C_BUS"
#define ENV_HARDWARE_CAMERA_TYPE     "SHELLCLAW_CAMERA_TYPE"

#define DEFAULT_LOCAL_ENDPOINT "http://127.0.0.1:8080/v1/chat/completions"
#define DEFAULT_CAMERA_TYPE "auto"
#define DEFAULT_CAMERA_RESOLUTION "640x480"
#define DEFAULT_CAMERA_QUALITY 75
#define DEFAULT_LOCAL_MODEL "tinyllama-1.1b-q4"

struct config {
	char *agent_model;
	int agent_max_tokens;
	double agent_temperature;
	int agent_max_tool_iterations;
	int agent_max_context_messages;
	char *agent_soul_path;
	char *agent_identity_path;
	char *agent_user_path;
	int agent_has_latitude;
	double agent_latitude;
	int agent_has_longitude;
	double agent_longitude;
	char *agent_country_code;
	char *provider_default;
	char *provider_anthropic_api_key_env;
	char *provider_openai_api_key_env;
	char *provider_openai_endpoint;
	char **provider_fallback_chain;
	int provider_fallback_chain_count;
	char *provider_local_endpoint;
	char *provider_local_model;
	int telegram_enabled;
	char *telegram_token_env;
	char **telegram_allowed_users;
	int telegram_allowed_users_count;
	int discord_enabled;
	char *discord_token_env;
	char **discord_allowed_user_ids;
	int discord_allowed_user_ids_count;
	char *memory_db_path;
	char *skills_dir;
	int workspace_only;
	char *workspace_path;
	int shell_timeout_sec;
	int sandbox_enabled;
	size_t sandbox_memory_max_bytes;
	char *sandbox_cpu_max;
	char *sandbox_cgroup_base;
	int gateway_enabled;
	char *gateway_host;
	int gateway_port;
	int gateway_allow_bind_all;
	int asap_enabled;
	char *asap_agent_urn;
	char *asap_agent_name;
	char *asap_description;
	char *asap_public_base_url;
	char *asap_registry_url;
	char **asap_skill_desc_ids;
	char **asap_skill_desc_texts;
	int asap_skill_desc_count;
	char *asap_revocation_list_url;
	int asap_client_timeout_sec;
	char **asap_trusted_senders;
	int asap_trusted_senders_count;
	int heartbeat_enabled;
	int heartbeat_interval_minutes;
	char *heartbeat_default_channel;
	char *brave_api_key_env;
	char *tavily_api_key_env;
	int hardware_enabled;
	char *hardware_board;
	char *hardware_class;
	char *hardware_model;
	char **hardware_io;
	int hardware_io_count;
	int hardware_has_i2c_bus;
	int hardware_i2c_bus;
	char *hardware_camera_type;
	char *hardware_camera_resolution;
	int hardware_camera_quality;
	int hardware_has_gpio_test_pin;
	int hardware_gpio_test_pin;
};

static void set_string(char **dst, const char *src)
{
	if (*dst) free(*dst);
	*dst = src ? strdup(src) : NULL;
}

char *config_expand_tilde(const char *path)
{
	if (!path || path[0] != '~') return path ? strdup(path) : NULL;
	const char *home = getenv("HOME");
	if (!home) home = "";
		if (path[1] == '\0' || path[1] == '/') {
			size_t hlen = strlen(home);
			size_t tail = strlen(path + 1);  /* skip '~' */
			char *out = malloc(hlen + tail + 1);
			if (!out) return NULL;
			memcpy(out, home, hlen);
			memcpy(out + hlen, path + 1, tail + 1);  /* includes NUL */
			return out;
		}
	return strdup(path);
}

static int parse_agent(const toml_table_t *root, config_t *cfg, char *errbuf, size_t errbufsz)
{
	const toml_table_t *agent = toml_table_in(root, "agent");
	if (!agent) {
		ERRBUF_COPY(errbuf, errbufsz, "missing [agent] section");
		return -1;
	}
	toml_datum_t d = toml_string_in(agent, "model");
	if (d.ok) {
		set_string(&cfg->agent_model, d.u.s);
		free(d.u.s);
	}
	d = toml_int_in(agent, "max_tokens");
	if (d.ok) cfg->agent_max_tokens = (int)d.u.i;
	d = toml_double_in(agent, "temperature");
	if (d.ok) cfg->agent_temperature = d.u.d;
	d = toml_int_in(agent, "max_tool_iterations");
	if (d.ok) cfg->agent_max_tool_iterations = (int)d.u.i;
	d = toml_int_in(agent, "max_context_messages");
	if (d.ok) cfg->agent_max_context_messages = (int)d.u.i;
	const toml_table_t *identity = toml_table_in(agent, "identity");
	if (identity) {
		d = toml_string_in(identity, "soul");
		if (d.ok) { set_string(&cfg->agent_soul_path, d.u.s); free(d.u.s); }
		d = toml_string_in(identity, "identity");
		if (d.ok) { set_string(&cfg->agent_identity_path, d.u.s); free(d.u.s); }
		d = toml_string_in(identity, "user");
		if (d.ok) { set_string(&cfg->agent_user_path, d.u.s); free(d.u.s); }
	}
	d = toml_double_in(agent, "latitude");
	if (d.ok) {
		cfg->agent_has_latitude = 1;
		cfg->agent_latitude = d.u.d;
	}
	d = toml_double_in(agent, "longitude");
	if (d.ok) {
		cfg->agent_has_longitude = 1;
		cfg->agent_longitude = d.u.d;
	}
	d = toml_string_in(agent, "country_code");
	if (d.ok) { set_string(&cfg->agent_country_code, d.u.s); free(d.u.s); }
	return 0;
}

static void trim_inplace(char *s)
{
	char *p = s;
	size_t len;
	while (*p == ' ' || *p == '\t')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		s[--len] = '\0';
}

/** Frees prior fallback_chain entries. */
static void fallback_chain_clear(config_t *cfg)
{
	int i;
	if (!cfg) return;
	for (i = 0; i < cfg->provider_fallback_chain_count; i++)
		free(cfg->provider_fallback_chain[i]);
	free(cfg->provider_fallback_chain);
	cfg->provider_fallback_chain = NULL;
	cfg->provider_fallback_chain_count = 0;
}

/** Default PRD fallback order: anthropic → openai → local. Returns -1 if allocation fails (cfg cleared). */
static int fallback_chain_set_defaults(config_t *cfg)
{
	static const char *names[] = { "anthropic", "openai", "local" };
	char **dup;
	size_t i;
	fallback_chain_clear(cfg);
	dup = calloc(3, sizeof(char *));
	if (!dup) return -1;
	for (i = 0; i < 3; i++) {
		dup[i] = strdup(names[i]);
		if (!dup[i]) {
			for (; i > 0;)
				free(dup[--i]);
			free(dup);
			fallback_chain_clear(cfg);
			return -1;
		}
	}
	cfg->provider_fallback_chain = dup;
	cfg->provider_fallback_chain_count = 3;
	return 0;
}

/** env SHELLCLAW_FALLBACK_CHAIN comma list; on OOM returns -1 and leaves prior chain. */
static int fallback_chain_apply_csv_override(config_t *cfg, const char *csv)
{
	char *work = NULL;
	char *save = NULL;
	char *tok;
	char **built = NULL;
	int n = 0;
	int capacity = 0;
	char **tmp = NULL;
	if (!cfg || !csv || csv[0] == '\0') return 0;
	work = strdup(csv);
	if (!work) return -1;
	for (tok = strtok_r(work, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
		char *dup;
		trim_inplace(tok);
		if (tok[0] == '\0') continue;
		if (n >= capacity) {
			int nc = capacity == 0 ? 8 : capacity * 2;
			tmp = realloc(built, (size_t)nc * sizeof(char *));
			if (!tmp) goto fail_partial;
			built = tmp;
			capacity = nc;
		}
		dup = strdup(tok);
		if (!dup) goto fail_partial;
		built[n++] = dup;
	}
	free(work);
	work = NULL;
	if (n == 0) {
		free(built);
		return 0;
	}
	tmp = realloc(built, (size_t)n * sizeof(char *));
	if (!tmp) goto fail_partial;
	built = tmp;
	fallback_chain_clear(cfg);
	cfg->provider_fallback_chain = built;
	cfg->provider_fallback_chain_count = n;
	return 0;
fail_partial:
	if (work) free(work);
	for (; n > 0;)
		free(built[--n]);
	free(built);
	return -1;
}

static int parse_providers(const toml_table_t *root, config_t *cfg, char *errbuf, size_t errbufsz)
{
	const toml_table_t *providers = toml_table_in(root, "providers");
	const toml_table_t *local_tbl;
	const toml_table_t *anth;
	const toml_table_t *openai;
	const toml_array_t *fb_arr;
	toml_datum_t d_def;
	toml_datum_t d_ep;
	int n_fb;
	char **parsed = NULL;
	if (!providers) return 0;
	d_def = toml_string_in(providers, "default");
	if (d_def.ok) { set_string(&cfg->provider_default, d_def.u.s); free(d_def.u.s); }
	anth = toml_table_in(providers, "anthropic");
	if (anth) {
		toml_datum_t d_anth = toml_string_in(anth, "api_key_env");
		if (d_anth.ok) { set_string(&cfg->provider_anthropic_api_key_env, d_anth.u.s); free(d_anth.u.s); }
	}
	openai = toml_table_in(providers, "openai");
	if (openai) {
		toml_datum_t d_oe = toml_string_in(openai, "api_key_env");
		if (d_oe.ok) { set_string(&cfg->provider_openai_api_key_env, d_oe.u.s); free(d_oe.u.s); }
		d_ep = toml_string_in(openai, "endpoint");
		if (d_ep.ok) { set_string(&cfg->provider_openai_endpoint, d_ep.u.s); free(d_ep.u.s); }
	}
	local_tbl = toml_table_in(providers, "local");
	if (local_tbl) {
		toml_datum_t d_lm;
		d_ep = toml_string_in(local_tbl, "endpoint");
		if (d_ep.ok) { set_string(&cfg->provider_local_endpoint, d_ep.u.s); free(d_ep.u.s); }
		d_lm = toml_string_in(local_tbl, "model");
		if (d_lm.ok) { set_string(&cfg->provider_local_model, d_lm.u.s); free(d_lm.u.s); }
	}
	fb_arr = toml_array_in(providers, "fallback_chain");
	if (fb_arr)
		n_fb = toml_array_nelem(fb_arr);
	else
		n_fb = 0;
	if (n_fb > 0) {
		int count = 0;
		parsed = calloc((size_t)n_fb, sizeof(char *));
		if (!parsed) {
			ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating fallback_chain");
			return -1;
		}
		for (int i = 0; i < n_fb; i++) {
			char *dup;
			toml_datum_t st = toml_string_at(fb_arr, i);
			if (!st.ok || !st.u.s || st.u.s[0] == '\0') {
				if (st.ok && st.u.s)
					free(st.u.s);
				continue;
			}
			dup = strdup(st.u.s);
			free(st.u.s);
			if (!dup) {
				for (int j = 0; j < count; j++)
					free(parsed[j]);
				free(parsed);
				ERRBUF_COPY(errbuf, errbufsz, "out of memory copying fallback_chain");
				return -1;
			}
			parsed[count++] = dup;
		}
		if (count > 0) {
			char **shrunk = realloc(parsed, (size_t)count * sizeof(char *));
			if (!shrunk) {
				for (int j = 0; j < count; j++)
					free(parsed[j]);
				free(parsed);
				ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating fallback_chain");
				return -1;
			}
			fallback_chain_clear(cfg);
			cfg->provider_fallback_chain = shrunk;
			cfg->provider_fallback_chain_count = count;
		} else {
			free(parsed);
		}
	}
	return 0;
}

static int parse_telegram(const toml_table_t *root, config_t *cfg, char *errbuf, size_t errbufsz)
{
	const toml_table_t *ch = toml_table_in(root, "channels");
	if (!ch) return 0;
	const toml_table_t *tg = toml_table_in(ch, "telegram");
	if (!tg) return 0;
	toml_datum_t d = toml_bool_in(tg, "enabled");
	if (d.ok) cfg->telegram_enabled = d.u.b;
	d = toml_string_in(tg, "token_env");
	if (d.ok) { set_string(&cfg->telegram_token_env, d.u.s); free(d.u.s); }
	const toml_array_t *arr = toml_array_in(tg, "allowed_users");
	if (arr) {
		int n = toml_array_nelem(arr);
		if (n > 0) {
			char **users = malloc((size_t)n * sizeof(char *));
			if (!users) {
				ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating telegram allowed_users");
				return -1;
			}
			for (int i = 0; i < n; i++) {
				toml_datum_t s = toml_string_at(arr, i);
				users[i] = s.ok ? s.u.s : NULL;
			}
			cfg->telegram_allowed_users = users;
			cfg->telegram_allowed_users_count = n;
		}
	}
	return 0;
}

static int parse_discord(const toml_table_t *root, config_t *cfg, char *errbuf, size_t errbufsz)
{
	const toml_table_t *ch = toml_table_in(root, "channels");
	const toml_table_t *dc;
	const toml_array_t *arr;
	int n;
	if (!ch) return 0;
	dc = toml_table_in(ch, "discord");
	if (!dc) return 0;
	toml_datum_t d = toml_bool_in(dc, "enabled");
	if (d.ok) cfg->discord_enabled = d.u.b;
	d = toml_string_in(dc, "token_env");
	if (d.ok) { set_string(&cfg->discord_token_env, d.u.s); free(d.u.s); }
	arr = toml_array_in(dc, "allowed_user_ids");
	if (!arr) return 0;
	n = toml_array_nelem(arr);
	if (n <= 0) return 0;
	{
		char **users = malloc((size_t)n * sizeof(char *));
		if (!users) {
			ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating discord allowed_user_ids");
			return -1;
		}
		for (int i = 0; i < n; i++) {
			toml_datum_t s = toml_string_at(arr, i);
			users[i] = s.ok ? s.u.s : NULL;
		}
		cfg->discord_allowed_user_ids = users;
		cfg->discord_allowed_user_ids_count = n;
	}
	return 0;
}

static int parse_gateway(const toml_table_t *root, config_t *cfg)
{
	const toml_table_t *gw = toml_table_in(root, "gateway");
	if (!gw) return 0;
	toml_datum_t d = toml_bool_in(gw, "enabled");
	if (d.ok) cfg->gateway_enabled = d.u.b;
	d = toml_string_in(gw, "host");
	if (d.ok) { set_string(&cfg->gateway_host, d.u.s); free(d.u.s); }
	d = toml_int_in(gw, "port");
	if (d.ok) cfg->gateway_port = (int)d.u.i;
	d = toml_bool_in(gw, "allow_bind_all");
	if (d.ok) cfg->gateway_allow_bind_all = d.u.b;
	return 0;
}

static void free_string_array(char **items, int count)
{
	int i;
	if (!items) return;
	for (i = 0; i < count; i++)
		free(items[i]);
	free(items);
}

static void free_asap_skill_descriptions(config_t *cfg)
{
	if (!cfg) return;
	free_string_array(cfg->asap_skill_desc_ids, cfg->asap_skill_desc_count);
	free_string_array(cfg->asap_skill_desc_texts, cfg->asap_skill_desc_count);
	cfg->asap_skill_desc_ids = NULL;
	cfg->asap_skill_desc_texts = NULL;
	cfg->asap_skill_desc_count = 0;
}

static void free_hardware_io(config_t *cfg)
{
	if (!cfg) return;
	free_string_array(cfg->hardware_io, cfg->hardware_io_count);
	cfg->hardware_io = NULL;
	cfg->hardware_io_count = 0;
}

static void free_asap_trusted_senders(config_t *cfg)
{
	int i;
	if (!cfg || !cfg->asap_trusted_senders) return;
	for (i = 0; i < cfg->asap_trusted_senders_count; i++)
		free(cfg->asap_trusted_senders[i]);
	free(cfg->asap_trusted_senders);
	cfg->asap_trusted_senders = NULL;
	cfg->asap_trusted_senders_count = 0;
}

static int parse_asap(const toml_table_t *root, config_t *cfg, char *errbuf, size_t errbufsz)
{
	const toml_table_t *asap = toml_table_in(root, "asap");
	const toml_array_t *arr;
	char **senders;
	int n;
	int i;
	int count;
	if (!asap) return 0;
	toml_datum_t d = toml_bool_in(asap, "enabled");
	if (d.ok) cfg->asap_enabled = d.u.b;
	d = toml_string_in(asap, "agent_urn");
	if (d.ok) { set_string(&cfg->asap_agent_urn, d.u.s); free(d.u.s); }
	d = toml_string_in(asap, "agent_name");
	if (d.ok) { set_string(&cfg->asap_agent_name, d.u.s); free(d.u.s); }
	d = toml_string_in(asap, "description");
	if (d.ok) { set_string(&cfg->asap_description, d.u.s); free(d.u.s); }
	d = toml_string_in(asap, "public_base_url");
	if (d.ok) { set_string(&cfg->asap_public_base_url, d.u.s); free(d.u.s); }
	{
		const toml_table_t *desc_tbl = toml_table_in(asap, "skill_descriptions");
		if (desc_tbl) {
			int desc_n = toml_table_nkval(desc_tbl);
			if (desc_n > 0) {
				char **ids = calloc((size_t)desc_n, sizeof(char *));
				char **texts = calloc((size_t)desc_n, sizeof(char *));
				int desc_count = 0;
				if (!ids || !texts) {
					free(ids);
					free(texts);
					ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating asap skill_descriptions");
					return -1;
				}
				for (int desc_i = 0; desc_i < desc_n && desc_count < MAX_ASAP_SKILL_DESCRIPTIONS; desc_i++) {
					const char *kid = toml_key_in(desc_tbl, desc_i);
					toml_datum_t val;
					if (!kid || !kid[0]) continue;
					val = toml_string_in(desc_tbl, kid);
					if (!val.ok || !val.u.s || !val.u.s[0]) {
						if (val.ok) free(val.u.s);
						continue;
					}
					ids[desc_count] = strdup(kid);
					texts[desc_count] = val.u.s;
					if (!ids[desc_count] || !texts[desc_count]) {
						free(val.u.s);
						if (ids[desc_count]) free(ids[desc_count]);
						int j;
						for (j = 0; j < desc_count; j++) {
							free(ids[j]);
							free(texts[j]);
						}
						free(ids);
						free(texts);
						ERRBUF_COPY(errbuf, errbufsz, "out of memory copying asap skill_descriptions");
						return -1;
					}
					desc_count++;
				}
				if (desc_count > 0) {
					free_asap_skill_descriptions(cfg);
					cfg->asap_skill_desc_ids = ids;
					cfg->asap_skill_desc_texts = texts;
					cfg->asap_skill_desc_count = desc_count;
				} else {
					free(ids);
					free(texts);
				}
			}
		}
	}
	d = toml_string_in(asap, "registry_url");
	if (d.ok) { set_string(&cfg->asap_registry_url, d.u.s); free(d.u.s); }
	d = toml_string_in(asap, "revocation_list_url");
	if (d.ok) { set_string(&cfg->asap_revocation_list_url, d.u.s); free(d.u.s); }
	d = toml_int_in(asap, "client_timeout_sec");
	if (d.ok && d.u.i > 0) cfg->asap_client_timeout_sec = (int)d.u.i;
	arr = toml_array_in(asap, "trusted_senders");
	if (!arr) return 0;
	n = toml_array_nelem(arr);
	if (n <= 0) return 0;
	senders = calloc((size_t)n, sizeof(char *));
	if (!senders) {
		ERRBUF_COPY(errbuf, errbufsz, "out of memory allocating asap trusted_senders");
		return -1;
	}
	count = 0;
	for (i = 0; i < n; i++) {
		toml_datum_t s = toml_string_at(arr, i);
		char *copy;
		if (!s.ok || !s.u.s) continue;
		copy = strdup(s.u.s);
		free(s.u.s);
		if (!copy) {
			int j;
			for (j = 0; j < count; j++)
				free(senders[j]);
			free(senders);
			ERRBUF_COPY(errbuf, errbufsz, "out of memory copying asap trusted_senders");
			return -1;
		}
		senders[count++] = copy;
	}
	if (count == 0) {
		free(senders);
		return 0;
	}
	free_asap_trusted_senders(cfg);
	cfg->asap_trusted_senders = senders;
	cfg->asap_trusted_senders_count = count;
	return 0;
}

static int parse_heartbeat(const toml_table_t *root, config_t *cfg)
{
	const toml_table_t *hb = toml_table_in(root, "heartbeat");
	if (!hb) return 0;
	toml_datum_t d = toml_bool_in(hb, "enabled");
	if (d.ok) cfg->heartbeat_enabled = d.u.b;
	d = toml_int_in(hb, "interval_minutes");
	if (d.ok) cfg->heartbeat_interval_minutes = (int)d.u.i;
	d = toml_string_in(hb, "default_channel");
	if (d.ok) { set_string(&cfg->heartbeat_default_channel, d.u.s); free(d.u.s); }
	return 0;
}

static int parse_web_search(const toml_table_t *root, config_t *cfg)
{
	const toml_table_t *ws = toml_table_in(root, "web_search");
	if (!ws) return 0;
	toml_datum_t d = toml_string_in(ws, "brave_api_key_env");
	if (d.ok) { set_string(&cfg->brave_api_key_env, d.u.s); free(d.u.s); }
	d = toml_string_in(ws, "tavily_api_key_env");
	if (d.ok) { set_string(&cfg->tavily_api_key_env, d.u.s); free(d.u.s); }
	return 0;
}

static int parse_hardware(const toml_table_t *root, config_t *cfg)
{
	const toml_table_t *hw = toml_table_in(root, "hardware");
	toml_datum_t d;
	if (!hw) return 0;
	d = toml_bool_in(hw, "enabled");
	if (d.ok) cfg->hardware_enabled = d.u.b;
	d = toml_string_in(hw, "board");
	if (d.ok) { set_string(&cfg->hardware_board, d.u.s); free(d.u.s); }
	d = toml_string_in(hw, "class");
	if (d.ok) { set_string(&cfg->hardware_class, d.u.s); free(d.u.s); }
	d = toml_string_in(hw, "model");
	if (d.ok) { set_string(&cfg->hardware_model, d.u.s); free(d.u.s); }
	{
		const toml_array_t *io_arr = toml_array_in(hw, "io");
		if (io_arr) {
			int io_n = toml_array_nelem(io_arr);
			if (io_n > 0 && io_n <= MAX_HARDWARE_IO_ENTRIES) {
				char **io = calloc((size_t)io_n, sizeof(char *));
				int io_count = 0;
				if (!io) return -1;
				for (int io_i = 0; io_i < io_n && io_count < MAX_HARDWARE_IO_ENTRIES; io_i++) {
					toml_datum_t s = toml_string_at(io_arr, io_i);
					if (!s.ok || !s.u.s || !s.u.s[0]) {
						if (s.ok) free(s.u.s);
						continue;
					}
					io[io_count] = s.u.s;
					io_count++;
				}
				if (io_count > 0) {
					free_hardware_io(cfg);
					cfg->hardware_io = io;
					cfg->hardware_io_count = io_count;
				} else {
					free(io);
				}
			}
		}
	}
	d = toml_int_in(hw, "i2c_bus");
	if (d.ok) {
		cfg->hardware_i2c_bus = (int)d.u.i;
		cfg->hardware_has_i2c_bus = 1;
	}
	d = toml_string_in(hw, "camera_type");
	if (d.ok) { set_string(&cfg->hardware_camera_type, d.u.s); free(d.u.s); }
	d = toml_string_in(hw, "camera_resolution");
	if (d.ok) { set_string(&cfg->hardware_camera_resolution, d.u.s); free(d.u.s); }
	d = toml_int_in(hw, "camera_quality");
	if (d.ok) cfg->hardware_camera_quality = (int)d.u.i;
	d = toml_int_in(hw, "gpio_test_pin");
	if (d.ok) {
		cfg->hardware_gpio_test_pin = (int)d.u.i;
		cfg->hardware_has_gpio_test_pin = 1;
	}
	return 0;
}

static int parse_memory_skills_sandbox(const toml_table_t *root, config_t *cfg)
{
	const toml_table_t *mem = toml_table_in(root, "memory");
	if (mem) {
		toml_datum_t d = toml_string_in(mem, "db_path");
		if (d.ok) { set_string(&cfg->memory_db_path, d.u.s); free(d.u.s); }
	}
	const toml_table_t *skills = toml_table_in(root, "skills");
	if (skills) {
		toml_datum_t d = toml_string_in(skills, "dir");
		if (d.ok) { set_string(&cfg->skills_dir, d.u.s); free(d.u.s); }
	}
	const toml_table_t *sandbox = toml_table_in(root, "sandbox");
	if (sandbox) {
		toml_datum_t d = toml_bool_in(sandbox, "workspace_only");
		if (d.ok) cfg->workspace_only = d.u.b;
		d = toml_string_in(sandbox, "workspace_path");
		if (d.ok) { set_string(&cfg->workspace_path, d.u.s); free(d.u.s); }
		d = toml_int_in(sandbox, "shell_timeout_sec");
		if (d.ok) cfg->shell_timeout_sec = (int)d.u.i;
		d = toml_bool_in(sandbox, "enabled");
		if (d.ok) cfg->sandbox_enabled = d.u.b;
		d = toml_int_in(sandbox, "memory_max_bytes");
		if (d.ok && d.u.i > 0) cfg->sandbox_memory_max_bytes = (size_t)d.u.i;
		d = toml_string_in(sandbox, "cpu_max");
		if (d.ok) { set_string(&cfg->sandbox_cpu_max, d.u.s); free(d.u.s); }
		d = toml_string_in(sandbox, "cgroup_base");
		if (d.ok) { set_string(&cfg->sandbox_cgroup_base, d.u.s); free(d.u.s); }
	}
	return 0;
}

static int parse_int_env(const char *v, int *out, int min_val, int max_val)
{
	if (!v || !out) return 0;
	char *end = NULL;
	long val = strtol(v, &end, 10);
	if (*end != '\0' || val < min_val || val > max_val) return 0;
	*out = (int)val;
	return 1;
}

static int parse_double_env(const char *v, double *out, double min_val, double max_val)
{
	if (!v || !out) return 0;
	char *end = NULL;
	double val = strtod(v, &end);
	if (*end != '\0' || val < min_val || val > max_val) return 0;
	*out = val;
	return 1;
}

static int apply_env_overrides(config_t *cfg)
{
	const char *v;
	v = getenv(ENV_AGENT_MODEL);
	if (v) set_string(&cfg->agent_model, v);
	v = getenv(ENV_AGENT_MAX_TOKENS);
	if (v) parse_int_env(v, &cfg->agent_max_tokens, 1, INT_MAX);
	v = getenv(ENV_AGENT_TEMPERATURE);
	if (v) parse_double_env(v, &cfg->agent_temperature, 0.0, 2.0);
	v = getenv(ENV_AGENT_MAX_TOOL_ITER);
	if (v) parse_int_env(v, &cfg->agent_max_tool_iterations, 1, 1000);
	v = getenv(ENV_AGENT_MAX_CTX_MSG);
	if (v) parse_int_env(v, &cfg->agent_max_context_messages, 1, 1000);
	v = getenv(ENV_AGENT_LATITUDE);
	if (v && parse_double_env(v, &cfg->agent_latitude, -90.0, 90.0))
		cfg->agent_has_latitude = 1;
	v = getenv(ENV_AGENT_LONGITUDE);
	if (v && parse_double_env(v, &cfg->agent_longitude, -180.0, 180.0))
		cfg->agent_has_longitude = 1;
	v = getenv(ENV_AGENT_COUNTRY_CODE);
	if (v && v[0]) set_string(&cfg->agent_country_code, v);
	v = getenv(ENV_MEMORY_DB_PATH);
	if (v) set_string(&cfg->memory_db_path, v);
	v = getenv(ENV_SKILLS_DIR);
	if (v) set_string(&cfg->skills_dir, v);
	v = getenv(ENV_OPENAI_ENDPOINT);
	if (v) set_string(&cfg->provider_openai_endpoint, v);
	v = getenv(ENV_DEFAULT_PROVIDER);
	if (v) set_string(&cfg->provider_default, v);
	v = getenv(ENV_LOCAL_ENDPOINT);
	if (v) set_string(&cfg->provider_local_endpoint, v);
	v = getenv(ENV_LOCAL_MODEL);
	if (v) set_string(&cfg->provider_local_model, v);
	v = getenv(ENV_FALLBACK_CHAIN);
	if (v && v[0]) {
		if (fallback_chain_apply_csv_override(cfg, v) != 0)
			return -1;
	}
	v = getenv(ENV_GATEWAY_ENABLED);
	if (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0))
		cfg->gateway_enabled = 1;
	else if (v && (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0))
		cfg->gateway_enabled = 0;
	v = getenv(ENV_GATEWAY_HOST);
	if (v) set_string(&cfg->gateway_host, v);
	v = getenv(ENV_GATEWAY_PORT);
	if (v) parse_int_env(v, &cfg->gateway_port, 1, 65535);
	v = getenv(ENV_GATEWAY_ALLOW_BIND);
	if (v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0))
		cfg->gateway_allow_bind_all = 1;
	else if (v && (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0))
		cfg->gateway_allow_bind_all = 0;
	v = getenv(ENV_ASAP_REGISTRY_URL);
	if (v) set_string(&cfg->asap_registry_url, v);
	v = getenv(ENV_ASAP_REVOCATION_LIST_URL);
	if (v) set_string(&cfg->asap_revocation_list_url, v);
	v = getenv(ENV_ASAP_DESCRIPTION);
	if (v && v[0]) set_string(&cfg->asap_description, v);
	v = getenv(ENV_ASAP_PUBLIC_BASE_URL);
	if (v && v[0]) set_string(&cfg->asap_public_base_url, v);
	v = getenv(ENV_HARDWARE_CLASS);
	if (v && v[0]) set_string(&cfg->hardware_class, v);
	v = getenv(ENV_HARDWARE_MODEL);
	if (v && v[0]) set_string(&cfg->hardware_model, v);
	v = getenv(ENV_HARDWARE_BOARD);
	if (v && v[0]) set_string(&cfg->hardware_board, v);
	v = getenv(ENV_HARDWARE_I2C_BUS);
	if (v && parse_int_env(v, &cfg->hardware_i2c_bus, 0, 255))
		cfg->hardware_has_i2c_bus = 1;
	v = getenv(ENV_HARDWARE_CAMERA_TYPE);
	if (v && v[0]) set_string(&cfg->hardware_camera_type, v);
	return 0;
}

static void expand_paths(config_t *cfg)
{
	char *s;
	if (cfg->agent_soul_path && cfg->agent_soul_path[0] == '~') {
		s = config_expand_tilde(cfg->agent_soul_path);
		if (s) { set_string(&cfg->agent_soul_path, s); free(s); }
	}
	if (cfg->agent_identity_path && cfg->agent_identity_path[0] == '~') {
		s = config_expand_tilde(cfg->agent_identity_path);
		if (s) { set_string(&cfg->agent_identity_path, s); free(s); }
	}
	if (cfg->agent_user_path && cfg->agent_user_path[0] == '~') {
		s = config_expand_tilde(cfg->agent_user_path);
		if (s) { set_string(&cfg->agent_user_path, s); free(s); }
	}
	if (cfg->memory_db_path && cfg->memory_db_path[0] == '~') {
		s = config_expand_tilde(cfg->memory_db_path);
		if (s) { set_string(&cfg->memory_db_path, s); free(s); }
	}
	if (cfg->skills_dir && cfg->skills_dir[0] == '~') {
		s = config_expand_tilde(cfg->skills_dir);
		if (s) { set_string(&cfg->skills_dir, s); free(s); }
	}
	if (cfg->workspace_path && cfg->workspace_path[0] == '~') {
		s = config_expand_tilde(cfg->workspace_path);
		if (s) { set_string(&cfg->workspace_path, s); free(s); }
	}
}

static int validate_required(const config_t *cfg, char *errbuf, size_t errbufsz)
{
	if (!cfg->agent_model || !cfg->agent_model[0]) {
		ERRBUF_COPY(errbuf, errbufsz, "agent.model is required");
		return -1;
	}
	return 0;
}

int config_load(const char *path, config_t **out, char *errbuf, size_t errbufsz)
{
	if (!path || !out) {
		ERRBUF_COPY(errbuf, errbufsz, "invalid arguments");
		return -1;
	}
	char *resolved = config_expand_tilde(path);
	if (!resolved) {
		ERRBUF_COPY(errbuf, errbufsz, "failed to expand path");
		return -1;
	}
	FILE *fp = fopen(resolved, "r");
	free(resolved);
	if (!fp) {
		if (errbuf && errbufsz > 0) {
			int n = snprintf(errbuf, errbufsz, "cannot open config file: %s", path);
			if (n < 0 || n >= (int)errbufsz) {
				if (errbufsz > 4) {
					memcpy(errbuf + errbufsz - 4, "...", 3);
					errbuf[errbufsz - 1] = '\0';
				}
			}
		}
		return -1;
	}
	char errbuf_toml[256];
	toml_table_t *tab = toml_parse_file(fp, errbuf_toml, sizeof(errbuf_toml));
	fclose(fp);
	if (!tab) {
		if (errbuf && errbufsz > 0) snprintf(errbuf, errbufsz, "config parse error: %s", errbuf_toml);
		return -1;
	}
	config_t *cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		toml_free(tab);
		ERRBUF_COPY(errbuf, errbufsz, "out of memory");
		return -1;
	}
	cfg->agent_max_tokens = DEFAULT_MAX_TOKENS;
	cfg->agent_temperature = DEFAULT_TEMPERATURE;
	cfg->agent_max_tool_iterations = DEFAULT_MAX_TOOL_ITERATIONS;
	cfg->agent_max_context_messages = DEFAULT_MAX_CONTEXT_MESSAGES;
	set_string(&cfg->provider_default, "anthropic");
	set_string(&cfg->provider_anthropic_api_key_env, "ANTHROPIC_API_KEY");
	set_string(&cfg->provider_openai_api_key_env, "OPENAI_API_KEY");
	set_string(&cfg->provider_openai_endpoint, "https://api.openai.com/v1/chat/completions");
	set_string(&cfg->provider_local_endpoint, DEFAULT_LOCAL_ENDPOINT);
	set_string(&cfg->provider_local_model, DEFAULT_LOCAL_MODEL);
	if (fallback_chain_set_defaults(cfg) != 0) {
		toml_free(tab);
		config_free(cfg);
		ERRBUF_COPY(errbuf, errbufsz, "out of memory");
		return -1;
	}
	set_string(&cfg->memory_db_path, "~/.shellclaw/memory.db");
	set_string(&cfg->skills_dir, "~/.shellclaw/skills");
	cfg->workspace_only = 1;
	cfg->gateway_port = DEFAULT_GATEWAY_PORT;
	set_string(&cfg->gateway_host, "127.0.0.1");
	set_string(&cfg->workspace_path, "~/.shellclaw");
	set_string(&cfg->asap_agent_urn, "urn:asap:agent:shellclaw");
	set_string(&cfg->asap_agent_name, "ShellClaw");
	set_string(&cfg->asap_description, DEFAULT_ASAP_DESCRIPTION);
	set_string(&cfg->asap_public_base_url, DEFAULT_ASAP_PUBLIC_BASE_URL);
	cfg->heartbeat_interval_minutes = 30;
	set_string(&cfg->heartbeat_default_channel, "cli");
	set_string(&cfg->brave_api_key_env, "BRAVE_API_KEY");
	set_string(&cfg->tavily_api_key_env, "TAVILY_API_KEY");
	cfg->shell_timeout_sec = DEFAULT_SHELL_TIMEOUT_SEC;
	cfg->hardware_enabled = 1;
	set_string(&cfg->hardware_camera_type, DEFAULT_CAMERA_TYPE);
	set_string(&cfg->hardware_camera_resolution, DEFAULT_CAMERA_RESOLUTION);
	cfg->hardware_camera_quality = DEFAULT_CAMERA_QUALITY;
	int err = parse_agent(tab, cfg, errbuf, errbufsz);
	if (err) goto fail;
	err = parse_providers(tab, cfg, errbuf, errbufsz);
	if (err) goto fail;
	err = parse_telegram(tab, cfg, errbuf, errbufsz);
	if (err) goto fail;
	err = parse_discord(tab, cfg, errbuf, errbufsz);
	if (err) goto fail;
	parse_memory_skills_sandbox(tab, cfg);
	parse_gateway(tab, cfg);
	err = parse_asap(tab, cfg, errbuf, errbufsz);
	if (err) goto fail;
	parse_heartbeat(tab, cfg);
	parse_web_search(tab, cfg);
	err = parse_hardware(tab, cfg);
	if (err) goto fail;
	toml_free(tab);
	tab = NULL;
	if (apply_env_overrides(cfg) != 0) {
		ERRBUF_COPY(errbuf, errbufsz, "invalid SHELLCLAW_FALLBACK_CHAIN");
		err = -1;
		goto fail;
	}
	expand_paths(cfg);
	err = validate_required(cfg, errbuf, errbufsz);
	if (err) goto fail;
	*out = cfg;
	return 0;
fail:
	if (tab) toml_free(tab);
	config_free(cfg);
	return -1;
}

void config_free(config_t *cfg)
{
	if (!cfg) return;
	set_string(&cfg->agent_model, NULL);
	set_string(&cfg->agent_soul_path, NULL);
	set_string(&cfg->agent_identity_path, NULL);
	set_string(&cfg->agent_user_path, NULL);
	cfg->agent_has_latitude = 0;
	cfg->agent_has_longitude = 0;
	set_string(&cfg->agent_country_code, NULL);
	set_string(&cfg->provider_default, NULL);
	set_string(&cfg->provider_anthropic_api_key_env, NULL);
	set_string(&cfg->provider_openai_api_key_env, NULL);
	set_string(&cfg->provider_openai_endpoint, NULL);
	set_string(&cfg->provider_local_endpoint, NULL);
	set_string(&cfg->provider_local_model, NULL);
	fallback_chain_clear(cfg);
	set_string(&cfg->telegram_token_env, NULL);
	if (cfg->telegram_allowed_users) {
		for (int i = 0; i < cfg->telegram_allowed_users_count; i++)
			free(cfg->telegram_allowed_users[i]);
		free(cfg->telegram_allowed_users);
		cfg->telegram_allowed_users = NULL;
		cfg->telegram_allowed_users_count = 0;
	}
	set_string(&cfg->discord_token_env, NULL);
	if (cfg->discord_allowed_user_ids) {
		for (int i = 0; i < cfg->discord_allowed_user_ids_count; i++)
			free(cfg->discord_allowed_user_ids[i]);
		free(cfg->discord_allowed_user_ids);
		cfg->discord_allowed_user_ids = NULL;
		cfg->discord_allowed_user_ids_count = 0;
	}
	set_string(&cfg->memory_db_path, NULL);
	set_string(&cfg->skills_dir, NULL);
	set_string(&cfg->workspace_path, NULL);
	set_string(&cfg->gateway_host, NULL);
	set_string(&cfg->asap_agent_urn, NULL);
	set_string(&cfg->asap_agent_name, NULL);
	set_string(&cfg->asap_description, NULL);
	set_string(&cfg->asap_public_base_url, NULL);
	free_asap_skill_descriptions(cfg);
	set_string(&cfg->asap_registry_url, NULL);
	set_string(&cfg->asap_revocation_list_url, NULL);
	free_asap_trusted_senders(cfg);
	set_string(&cfg->heartbeat_default_channel, NULL);
	set_string(&cfg->brave_api_key_env, NULL);
	set_string(&cfg->tavily_api_key_env, NULL);
	set_string(&cfg->sandbox_cpu_max, NULL);
	set_string(&cfg->sandbox_cgroup_base, NULL);
	set_string(&cfg->hardware_board, NULL);
	set_string(&cfg->hardware_class, NULL);
	set_string(&cfg->hardware_model, NULL);
	free_hardware_io(cfg);
	set_string(&cfg->hardware_camera_type, NULL);
	set_string(&cfg->hardware_camera_resolution, NULL);
	free(cfg);
}

const char *config_agent_model(const config_t *c) { return c ? c->agent_model : NULL; }
int config_agent_max_tokens(const config_t *c) { return c ? c->agent_max_tokens : 0; }
double config_agent_temperature(const config_t *c) { return c ? c->agent_temperature : 0.0; }
int config_agent_max_tool_iterations(const config_t *c) { return c ? c->agent_max_tool_iterations : 0; }
int config_agent_max_context_messages(const config_t *c) { return c ? c->agent_max_context_messages : 0; }
const char *config_agent_soul_path(const config_t *c) { return c ? c->agent_soul_path : NULL; }
const char *config_agent_identity_path(const config_t *c) { return c ? c->agent_identity_path : NULL; }
const char *config_agent_user_path(const config_t *c) { return c ? c->agent_user_path : NULL; }

int config_agent_has_latitude(const config_t *c) { return c ? c->agent_has_latitude : 0; }

double config_agent_latitude(const config_t *c)
{
	return c && c->agent_has_latitude ? c->agent_latitude : 0.0;
}

int config_agent_has_longitude(const config_t *c) { return c ? c->agent_has_longitude : 0; }

double config_agent_longitude(const config_t *c)
{
	return c && c->agent_has_longitude ? c->agent_longitude : 0.0;
}

const char *config_agent_country_code(const config_t *c)
{
	return c ? c->agent_country_code : NULL;
}

const char *config_default_provider(const config_t *c) { return c ? c->provider_default : NULL; }
const char *config_provider_anthropic_api_key_env(const config_t *c) { return c ? c->provider_anthropic_api_key_env : NULL; }
const char *config_provider_openai_api_key_env(const config_t *c) { return c ? c->provider_openai_api_key_env : NULL; }
const char *config_provider_openai_endpoint(const config_t *c) { return c ? c->provider_openai_endpoint : NULL; }

int config_provider_fallback_chain_count(const config_t *c)
{
	return c ? c->provider_fallback_chain_count : 0;
}

const char *config_provider_fallback_chain_entry(const config_t *c, int index)
{
	if (!c || !c->provider_fallback_chain || index < 0 || index >= c->provider_fallback_chain_count)
		return NULL;
	return c->provider_fallback_chain[index];
}

const char *config_provider_local_endpoint(const config_t *c)
{
	if (!c || !c->provider_local_endpoint || c->provider_local_endpoint[0] == '\0')
		return DEFAULT_LOCAL_ENDPOINT;
	return c->provider_local_endpoint;
}

const char *config_provider_local_model(const config_t *c)
{
	if (!c || !c->provider_local_model || c->provider_local_model[0] == '\0')
		return DEFAULT_LOCAL_MODEL;
	return c->provider_local_model;
}

int config_telegram_enabled(const config_t *c) { return c ? c->telegram_enabled : 0; }
const char *config_telegram_token_env(const config_t *c) { return c ? c->telegram_token_env : NULL; }
int config_telegram_allowed_users_count(const config_t *c) { return c ? c->telegram_allowed_users_count : 0; }
const char *config_telegram_allowed_user(const config_t *c, int index) {
	if (!c || !c->telegram_allowed_users || index < 0 || index >= c->telegram_allowed_users_count) return NULL;
	return c->telegram_allowed_users[index];
}

int config_discord_enabled(const config_t *c) { return c ? c->discord_enabled : 0; }

const char *config_discord_token_env(const config_t *c)
{
	if (!c || !c->discord_token_env || !c->discord_token_env[0])
		return "DISCORD_BOT_TOKEN";
	return c->discord_token_env;
}

int config_discord_allowed_user_ids_count(const config_t *c)
{
	return c ? c->discord_allowed_user_ids_count : 0;
}

const char *config_discord_allowed_user_id(const config_t *c, int index)
{
	if (!c || !c->discord_allowed_user_ids || index < 0 ||
	    index >= c->discord_allowed_user_ids_count)
		return NULL;
	return c->discord_allowed_user_ids[index];
}

const char *config_memory_db_path(const config_t *c) { return c ? c->memory_db_path : NULL; }
const char *config_skills_dir(const config_t *c) { return c ? c->skills_dir : NULL; }
int config_workspace_only(const config_t *c) { return c ? c->workspace_only : 0; }
const char *config_workspace_path(const config_t *c) { return c ? c->workspace_path : NULL; }
int config_shell_timeout_sec(const config_t *c) { return c && c->shell_timeout_sec > 0 ? c->shell_timeout_sec : DEFAULT_SHELL_TIMEOUT_SEC; }
int config_gateway_enabled(const config_t *c) { return c ? c->gateway_enabled : 0; }
const char *config_gateway_host(const config_t *c) { return c && c->gateway_host ? c->gateway_host : "127.0.0.1"; }
int config_gateway_port(const config_t *c) { return c && c->gateway_port > 0 ? c->gateway_port : DEFAULT_GATEWAY_PORT; }
int config_gateway_allow_bind_all(const config_t *c) { return c ? c->gateway_allow_bind_all : 0; }
int config_asap_enabled(const config_t *c) { return c ? c->asap_enabled : 0; }
const char *config_asap_agent_urn(const config_t *c) { return c && c->asap_agent_urn ? c->asap_agent_urn : "urn:asap:agent:shellclaw"; }
const char *config_asap_agent_name(const config_t *c) { return c && c->asap_agent_name ? c->asap_agent_name : "ShellClaw"; }

const char *config_asap_description(const config_t *c)
{
	if (!c || !c->asap_description || c->asap_description[0] == '\0')
		return DEFAULT_ASAP_DESCRIPTION;
	return c->asap_description;
}

const char *config_asap_public_base_url(const config_t *c)
{
	if (!c || !c->asap_public_base_url || c->asap_public_base_url[0] == '\0')
		return DEFAULT_ASAP_PUBLIC_BASE_URL;
	return c->asap_public_base_url;
}

const char *config_asap_skill_description(const config_t *c, const char *skill_id)
{
	int i;
	if (!c || !skill_id || !skill_id[0] || !c->asap_skill_desc_ids)
		return NULL;
	for (i = 0; i < c->asap_skill_desc_count; i++) {
		if (c->asap_skill_desc_ids[i] && strcmp(c->asap_skill_desc_ids[i], skill_id) == 0)
			return c->asap_skill_desc_texts[i];
	}
	return NULL;
}

const char *config_asap_registry_url(const config_t *c) { return c ? c->asap_registry_url : NULL; }
const char *config_asap_revocation_list_url(const config_t *c) { return c ? c->asap_revocation_list_url : NULL; }
int config_asap_client_timeout_sec(const config_t *c) { if (!c || c->asap_client_timeout_sec <= 0) return 30; return c->asap_client_timeout_sec; }
int config_asap_trusted_senders_count(const config_t *c)
{
	return c ? c->asap_trusted_senders_count : 0;
}
const char *config_asap_trusted_sender(const config_t *c, int index)
{
	if (!c || !c->asap_trusted_senders || index < 0 || index >= c->asap_trusted_senders_count)
		return NULL;
	return c->asap_trusted_senders[index];
}
int config_heartbeat_enabled(const config_t *c) { return c ? c->heartbeat_enabled : 0; }
int config_heartbeat_interval_minutes(const config_t *c) { return c && c->heartbeat_interval_minutes > 0 ? c->heartbeat_interval_minutes : 30; }
const char *config_heartbeat_default_channel(const config_t *c) { return c && c->heartbeat_default_channel ? c->heartbeat_default_channel : "cli"; }
const char *config_brave_api_key_env(const config_t *c) { return c && c->brave_api_key_env ? c->brave_api_key_env : "BRAVE_API_KEY"; }
const char *config_tavily_api_key_env(const config_t *c) { return c && c->tavily_api_key_env ? c->tavily_api_key_env : "TAVILY_API_KEY"; }
int config_sandbox_enabled(const config_t *c) { return c ? c->sandbox_enabled : 0; }
size_t config_sandbox_memory_max_bytes(const config_t *c) { return c ? c->sandbox_memory_max_bytes : 0; }
const char *config_sandbox_cpu_max(const config_t *c) { return c ? c->sandbox_cpu_max : NULL; }
const char *config_sandbox_cgroup_base(const config_t *c) { return c ? c->sandbox_cgroup_base : NULL; }

int config_hardware_enabled(const config_t *c)
{
	return c ? c->hardware_enabled : 1;
}

const char *config_hardware_board(const config_t *c)
{
	return c ? c->hardware_board : NULL;
}

const char *config_hardware_class(const config_t *c)
{
	return c ? c->hardware_class : NULL;
}

const char *config_hardware_model(const config_t *c)
{
	return c ? c->hardware_model : NULL;
}

int config_hardware_io_count(const config_t *c)
{
	return c ? c->hardware_io_count : 0;
}

const char *config_hardware_io_entry(const config_t *c, int index)
{
	if (!c || !c->hardware_io || index < 0 || index >= c->hardware_io_count)
		return NULL;
	return c->hardware_io[index];
}

int config_hardware_has_i2c_bus(const config_t *c)
{
	return c ? c->hardware_has_i2c_bus : 0;
}

int config_hardware_i2c_bus(const config_t *c)
{
	return c ? c->hardware_i2c_bus : 0;
}

const char *config_hardware_camera_type(const config_t *c)
{
	if (!c || !c->hardware_camera_type || c->hardware_camera_type[0] == '\0')
		return DEFAULT_CAMERA_TYPE;
	return c->hardware_camera_type;
}

const char *config_hardware_camera_resolution(const config_t *c)
{
	if (!c || !c->hardware_camera_resolution || c->hardware_camera_resolution[0] == '\0')
		return DEFAULT_CAMERA_RESOLUTION;
	return c->hardware_camera_resolution;
}

int config_hardware_camera_quality(const config_t *c)
{
	if (!c || c->hardware_camera_quality <= 0)
		return DEFAULT_CAMERA_QUALITY;
	return c->hardware_camera_quality;
}

int config_hardware_has_gpio_test_pin(const config_t *c)
{
	return c ? c->hardware_has_gpio_test_pin : 0;
}

int config_hardware_gpio_test_pin(const config_t *c)
{
	return c ? c->hardware_gpio_test_pin : 0;
}

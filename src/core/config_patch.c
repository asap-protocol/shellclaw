/**
 * @file config_patch.c
 * @brief Patch on-disk TOML from dashboard JSON updates.
 */
#define _POSIX_C_SOURCE 200809L

#include "core/config_patch.h"
#include "core/config.h"
#include "cJSON.h"
#include <ctype.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATCH_ERR(errbuf, errbufsz, msg)                                       \
	do {                                                                   \
		if ((errbuf) && (errbufsz) > 0)                                \
			snprintf((errbuf), (errbufsz), "%s", (msg));           \
	} while (0)

static char *read_file(const char *path, size_t *out_len, char *errbuf, size_t errbufsz)
{
	FILE *f;
	char *buf;
	long n;
	size_t got;
	if (!path || !out_len) {
		PATCH_ERR(errbuf, errbufsz, "invalid arguments");
		return NULL;
	}
	f = fopen(path, "r");
	if (!f) {
		PATCH_ERR(errbuf, errbufsz, "cannot open config file");
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		PATCH_ERR(errbuf, errbufsz, "cannot read config file");
		return NULL;
	}
	n = ftell(f);
	if (n < 0) {
		fclose(f);
		PATCH_ERR(errbuf, errbufsz, "cannot read config file");
		return NULL;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		PATCH_ERR(errbuf, errbufsz, "cannot read config file");
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		PATCH_ERR(errbuf, errbufsz, "out of memory");
		return NULL;
	}
	got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	if (got != (size_t)n) {
		free(buf);
		PATCH_ERR(errbuf, errbufsz, "cannot read config file");
		return NULL;
	}
	buf[got] = '\0';
	*out_len = got;
	return buf;
}

static int buf_reserve(char **buf, size_t *len, size_t *cap, size_t extra)
{
	size_t need;
	size_t new_cap;
	char *grown;
	if (!buf || !*buf || !len || !cap)
		return -1;
	need = *len + extra + 1;
	if (need <= *cap)
		return 0;
	new_cap = (*cap == 0) ? need : *cap;
	while (new_cap < need) {
		if (new_cap > ((size_t)-1) / 2)
			return -1;
		new_cap *= 2;
	}
	grown = realloc(*buf, new_cap);
	if (!grown)
		return -1;
	*buf = grown;
	*cap = new_cap;
	return 0;
}

static int append_section_key(char **buf, size_t *len, size_t *cap, const char *section,
                              const char *key, const char *line_value)
{
	char extra[768];
	int n;
	if (!buf || !*buf || !len || !cap || !section || !key || !line_value)
		return -1;
	n = snprintf(extra, sizeof(extra), "\n[%s]\n%s = %s\n", section, key, line_value);
	if (n < 0 || (size_t)n >= sizeof(extra))
		return -1;
	if (buf_reserve(buf, len, cap, (size_t)n) != 0)
		return -1;
	memcpy(*buf + *len, extra, (size_t)n);
	*len += (size_t)n;
	(*buf)[*len] = '\0';
	return 0;
}

static int escape_toml_string(const char *in, char **out)
{
	size_t cap;
	size_t len;
	size_t i;
	if (!in || !out)
		return -1;
	cap = strlen(in) * 2 + 3;
	*out = malloc(cap);
	if (!*out)
		return -1;
	(*out)[0] = '"';
	len = 1;
	for (i = 0; in[i]; i++) {
		char extra = 0;
		if (in[i] == '"' || in[i] == '\\')
			extra = in[i];
		else if (in[i] == '\n')
			extra = 'n';
		else if (in[i] == '\r')
			extra = 'r';
		else if (in[i] == '\t')
			extra = 't';
		if (extra != 0) {
			if (len + 2 >= cap) {
				char *grown;
				cap *= 2;
				grown = realloc(*out, cap);
				if (!grown) {
					free(*out);
					*out = NULL;
					return -1;
				}
				*out = grown;
			}
			(*out)[len++] = '\\';
			(*out)[len++] = extra;
			continue;
		}
		if (len + 1 >= cap) {
			char *grown;
			cap *= 2;
			grown = realloc(*out, cap);
			if (!grown) {
				free(*out);
				*out = NULL;
				return -1;
			}
			*out = grown;
		}
		(*out)[len++] = in[i];
	}
	(*out)[len++] = '"';
	(*out)[len] = '\0';
	return 0;
}

static int section_header_closed(const char *after)
{
	if (!after)
		return 0;
	while (*after == ' ' || *after == '\t')
		after++;
	if (*after == '#' || *after == '\0' || *after == '\r' || *after == '\n')
		return 1;
	return 0;
}

static const char *find_section(const char *content, const char *section)
{
	char marker[128];
	size_t marker_len;
	const char *p;
	if (!content || !section)
		return NULL;
	snprintf(marker, sizeof(marker), "[%s]", section);
	marker_len = strlen(marker);
	for (p = content; *p; p++) {
		if (strncmp(p, marker, marker_len) != 0)
			continue;
		if (p != content && p[-1] != '\n')
			continue;
		if (!section_header_closed(p + marker_len))
			continue;
		return p;
	}
	return NULL;
}

static const char *section_end(const char *section_start)
{
	const char *p;
	if (!section_start)
		return NULL;
	p = strchr(section_start + 1, '\n');
	if (!p)
		return section_start + strlen(section_start);
	for (; *p; p++) {
		if (*p == '[' && (p == section_start || p[-1] == '\n'))
			return p;
	}
	return section_start + strlen(section_start);
}

static const char *find_key_line(const char *sec_start, const char *sec_end,
                                 const char *key, size_t *line_len)
{
	size_t key_len;
	const char *p;
	if (!sec_start || !sec_end || !key || !line_len)
		return NULL;
	key_len = strlen(key);
	for (p = sec_start; p < sec_end; p++) {
		const char *line_end = strchr(p, '\n');
		if (!line_end || line_end > sec_end)
			line_end = sec_end;
		{
			const char *key_at = p;
			size_t span;
			while (key_at < line_end && (*key_at == ' ' || *key_at == '\t'))
				key_at++;
			span = (size_t)(line_end - key_at);
			while (span > 0 && isspace((unsigned char)key_at[span - 1]))
				span--;
		if (span > key_len) {
			const char *after_key = key_at + key_len;
			while (after_key < line_end &&
			       (*after_key == ' ' || *after_key == '\t'))
				after_key++;
			if (strncmp(key_at, key, key_len) == 0 && after_key < line_end &&
			    *after_key == '=') {
				*line_len = (size_t)(line_end - p);
				if (*line_end == '\n')
					(*line_len)++;
				return p;
			}
		}
		}
		if (!*line_end)
			break;
		p = line_end;
	}
	return NULL;
}

static int splice_text(char **content, size_t *len, size_t *cap, size_t off,
                       size_t old_len, const char *insert, size_t insert_len)
{
	size_t suffix_len;
	char *next;
	if (!content || !*content || !len || !cap || !insert)
		return -1;
	if (off > *len || old_len > *len - off)
		return -1;
	suffix_len = *len - off - old_len;
	next = malloc(off + insert_len + suffix_len + 1);
	if (!next)
		return -1;
	memcpy(next, *content, off);
	memcpy(next + off, insert, insert_len);
	memcpy(next + off + insert_len, *content + off + old_len, suffix_len + 1);
	free(*content);
	*content = next;
	*len = off + insert_len + suffix_len;
	if (*len + 1 > *cap)
		*cap = *len + 1;
	return 0;
}

static int patch_key_line(char **content, size_t *len, size_t *cap, const char *section,
                          const char *key, const char *line_value)
{
	const char *sec;
	const char *sec_end;
	const char *line;
	size_t line_len;
	char insert_line[512];
	int n;
	if (!content || !*content || !len || !cap || !section || !key || !line_value)
		return -1;
	n = snprintf(insert_line, sizeof(insert_line), "%s = %s\n", key, line_value);
	if (n < 0 || (size_t)n >= sizeof(insert_line))
		return -1;
	sec = find_section(*content, section);
	if (!sec)
		return append_section_key(content, len, cap, section, key, line_value);
	sec_end = section_end(sec);
	line = find_key_line(sec, sec_end, key, &line_len);
	if (!line) {
		return splice_text(content, len, cap, (size_t)(sec_end - *content), 0,
		                   insert_line, (size_t)n);
	}
	return splice_text(content, len, cap, (size_t)(line - *content), line_len,
	                   insert_line, (size_t)n);
}

static int patch_string_field(char **content, size_t *len, size_t *cap, const char *section,
                              const char *key, const char *value)
{
	char *escaped;
	int rc;
	if (!value)
		return 0;
	if (escape_toml_string(value, &escaped) != 0)
		return -1;
	rc = patch_key_line(content, len, cap, section, key, escaped);
	free(escaped);
	return rc;
}

static int patch_int_field(char **content, size_t *len, size_t *cap, const char *section,
                           const char *key, int value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", value);
	return patch_key_line(content, len, cap, section, key, buf);
}

static int patch_double_field(char **content, size_t *len, size_t *cap, const char *section,
                              const char *key, double value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%g", value);
	return patch_key_line(content, len, cap, section, key, buf);
}

static int reject_wrong_type(const cJSON *item, int expect_string, const char *field,
                             char *errbuf, size_t errbufsz)
{
	int ok;
	if (!item)
		return 0;
	ok = expect_string ? cJSON_IsString(item) : cJSON_IsNumber(item);
	if (ok)
		return 0;
	if (errbuf && errbufsz > 0)
		snprintf(errbuf, errbufsz, "field \"%s\" must be a JSON %s", field,
		         expect_string ? "string" : "number");
	return -1;
}

static int apply_dashboard_fields(cJSON *root, char **content, size_t *len, size_t *cap,
                                  char *errbuf, size_t errbufsz)
{
	cJSON *model = cJSON_GetObjectItem(root, "model");
	cJSON *max_tokens = cJSON_GetObjectItem(root, "max_tokens");
	cJSON *temperature = cJSON_GetObjectItem(root, "temperature");
	cJSON *gateway_host = cJSON_GetObjectItem(root, "gateway_host");
	cJSON *gateway_port = cJSON_GetObjectItem(root, "gateway_port");
	if (reject_wrong_type(model, 1, "model", errbuf, errbufsz) != 0 ||
	    reject_wrong_type(max_tokens, 0, "max_tokens", errbuf, errbufsz) != 0 ||
	    reject_wrong_type(temperature, 0, "temperature", errbuf, errbufsz) != 0 ||
	    reject_wrong_type(gateway_host, 1, "gateway_host", errbuf, errbufsz) != 0 ||
	    reject_wrong_type(gateway_port, 0, "gateway_port", errbuf, errbufsz) != 0)
		return -1;
	if (model &&
	    patch_string_field(content, len, cap, "agent", "model", model->valuestring) != 0)
		return -1;
	if (max_tokens &&
	    patch_int_field(content, len, cap, "agent", "max_tokens", max_tokens->valueint) != 0)
		return -1;
	if (temperature &&
	    patch_double_field(content, len, cap, "agent", "temperature",
	                       temperature->valuedouble) != 0)
		return -1;
	if (gateway_host &&
	    patch_string_field(content, len, cap, "gateway", "host",
	                       gateway_host->valuestring) != 0)
		return -1;
	if (gateway_port &&
	    patch_int_field(content, len, cap, "gateway", "port", gateway_port->valueint) != 0)
		return -1;
	return 0;
}

static int write_all_fd(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static int validate_patched_toml(const char *config_path, const char *content, size_t len,
                                 char *errbuf, size_t errbufsz)
{
	char path_copy[PATH_MAX];
	char tmp_path[PATH_MAX];
	char *dir;
	config_t *cfg = NULL;
	int fd;
	int n;

	if (snprintf(path_copy, sizeof(path_copy), "%s", config_path) >= (int)sizeof(path_copy)) {
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	dir = dirname(path_copy);
	if (!dir || dir[0] == '\0') {
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	n = snprintf(tmp_path, sizeof(tmp_path), "%s/.sc-patch-XXXXXX", dir);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	fd = mkstemp(tmp_path);
	if (fd < 0) {
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (write_all_fd(fd, content, len) != 0 || fsync(fd) != 0) {
		close(fd);
		unlink(tmp_path);
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	if (close(fd) != 0) {
		unlink(tmp_path);
		PATCH_ERR(errbuf, errbufsz, "failed to validate patched config");
		return -1;
	}
	if (config_load(tmp_path, &cfg, errbuf, errbufsz) != 0) {
		unlink(tmp_path);
		return -1;
	}
	config_free(cfg);
	unlink(tmp_path);
	return 0;
}

int config_patch_dashboard_json(const char *config_path, const char *json_body, char **out_toml,
                                size_t *out_len, char *errbuf, size_t errbufsz)
{
	cJSON *root;
	size_t cap;
	char *content;
	size_t len;
	if (!config_path || !json_body || !out_toml || !out_len) {
		PATCH_ERR(errbuf, errbufsz, "invalid arguments");
		return -1;
	}
	*out_toml = NULL;
	*out_len = 0;
	root = cJSON_Parse(json_body);
	if (!root || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		PATCH_ERR(errbuf, errbufsz, "invalid JSON body");
		return -1;
	}
	content = read_file(config_path, &len, errbuf, errbufsz);
	if (!content) {
		cJSON_Delete(root);
		return -1;
	}
	cap = len + 1;
	if (apply_dashboard_fields(root, &content, &len, &cap, errbuf, errbufsz) != 0) {
		if (!errbuf || errbufsz == 0 || errbuf[0] == '\0')
			PATCH_ERR(errbuf, errbufsz, "failed to patch config fields");
		free(content);
		cJSON_Delete(root);
		return -1;
	}
	if (validate_patched_toml(config_path, content, len, errbuf, errbufsz) != 0) {
		free(content);
		cJSON_Delete(root);
		return -1;
	}
	*out_toml = content;
	*out_len = len;
	cJSON_Delete(root);
	return 0;
}

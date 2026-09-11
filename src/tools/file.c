/**
 * @file file.c
 * @brief File tool: read_file, write_file, list_dir with workspace boundary check.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "tools/tool.h"
#include "tools/file.h"
#include "core/config.h"
#include "cJSON.h"
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE_SIZE (256 * 1024)
#define LIST_ENTRIES_MAX 200

static const char FILE_PARAMS[] =
	"{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":[\"read_file\",\"write_file\",\"list_dir\"]},\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"operation\",\"path\"]}";

static const config_t *g_file_cfg;

void tool_file_set_config(const config_t *cfg)
{
	g_file_cfg = cfg;
}

static int resolved_is_under_workspace(const char *resolved)
{
	char ws_resolved[PATH_MAX];
	const char *workspace;
	size_t ws_len;

	if (!resolved || !g_file_cfg) return 0;
	workspace = config_workspace_path(g_file_cfg);
	if (!workspace || workspace[0] == '\0') return 0;
	if (realpath(workspace, ws_resolved) == NULL) return 0;
	ws_len = strlen(ws_resolved);
	if (strncmp(resolved, ws_resolved, ws_len) != 0) return 0;
	if (resolved[ws_len] != '\0' && resolved[ws_len] != '/') return 0;
	return 1;
}

static int path_within_workspace(const char *path, char *resolved, size_t resolved_size)
{
	if (!path || path[0] == '\0') return 0;
	if (!g_file_cfg || !config_workspace_only(g_file_cfg)) {
		snprintf(resolved, resolved_size, "%s", path);
		return 1;
	}
	const char *workspace = config_workspace_path(g_file_cfg);
	if (!workspace || workspace[0] == '\0') {
		return 0;  /* Deny: cannot validate without workspace path */
	}
	if (realpath(workspace, resolved) == NULL) return 0;
	if (realpath(path, resolved) != NULL)
		return resolved_is_under_workspace(resolved);
	char path_copy[PATH_MAX];
	snprintf(path_copy, sizeof(path_copy), "%s", path);
	for (;;) {
		char *dir = dirname(path_copy);
		if (!dir || dir[0] == '\0') break;
		if (realpath(dir, resolved) != NULL)
			return resolved_is_under_workspace(resolved);
		if (strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0) break;
		snprintf(path_copy, sizeof(path_copy), "%s", dir);
	}
	return 0;
}

/*
 * Map a workspace write to the intended path. Ancestor lookup in
 * path_within_workspace is only a membership check: using that ancestor as
 * the fopen target truncates an existing file treated as a directory, or
 * overwrites a same-named file in a parent when intermediate dirs are missing.
 */
static int resolve_workspace_write_path(const char *path, char *safe_path, size_t cap)
{
	char parent[PATH_MAX];
	char path_copy[PATH_MAX];
	struct stat st;
	const char *base;
	char *dir;
	int n;

	if (realpath(path, safe_path) != NULL) {
		if (stat(safe_path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
		return resolved_is_under_workspace(safe_path);
	}
	if (snprintf(path_copy, sizeof(path_copy), "%s", path) >= (int)sizeof(path_copy))
		return 0;
	dir = dirname(path_copy);
	if (!dir || realpath(dir, parent) == NULL) return 0;
	if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
	if (!resolved_is_under_workspace(parent)) return 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
		return 0;
	n = snprintf(safe_path, cap, "%s/%s", parent, base);
	return n > 0 && (size_t)n < cap;
}

static int file_read(const char *path, char *result_buf, size_t max_len)
{
	char resolved[PATH_MAX];
	int ws_only;
	if (!path_within_workspace(path, resolved, sizeof(resolved))) {
		snprintf(result_buf, max_len, "{\"error\":\"path outside workspace\"}");
		return -1;
	}
	ws_only = g_file_cfg ? config_workspace_only(g_file_cfg) : 0;
	if (ws_only && realpath(path, resolved) == NULL) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot open file\"}");
		return -1;
	}
	FILE *f = fopen(resolved, "rb");
	if (!f) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot open file\"}");
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || sz > (long)MAX_FILE_SIZE) {
		fclose(f);
		snprintf(result_buf, max_len, "{\"error\":\"file too large\"}");
		return -1;
	}
	size_t to_read = (size_t)sz;
	if (to_read >= max_len - 1) to_read = max_len - 1;
	size_t n = fread(result_buf, 1, to_read, f);
	fclose(f);
	result_buf[n] = '\0';
	return 0;
}

static int file_write(const char *path, const char *content, char *result_buf, size_t max_len)
{
	char resolved[PATH_MAX];
	if (!path_within_workspace(path, resolved, sizeof(resolved))) {
		snprintf(result_buf, max_len, "{\"error\":\"path outside workspace\"}");
		return -1;
	}
	int ws_only = g_file_cfg ? config_workspace_only(g_file_cfg) : 0;
	char safe_path[PATH_MAX];
	if (!ws_only) {
		snprintf(safe_path, sizeof(safe_path), "%s", path);
	} else if (!resolve_workspace_write_path(path, safe_path, sizeof(safe_path))) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot write file\"}");
		return -1;
	}
	FILE *f = fopen(safe_path, "w");
	if (!f) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot write file\"}");
		return -1;
	}
	if (content) {
		size_t len = strlen(content);
		if (fwrite(content, 1, len, f) != len) {
			fclose(f);
			snprintf(result_buf, max_len, "{\"error\":\"write failed\"}");
			return -1;
		}
	}
	fclose(f);
	snprintf(result_buf, max_len, "{\"status\":\"ok\"}");
	return 0;
}

static int file_list(const char *path, char *result_buf, size_t max_len)
{
	char resolved[PATH_MAX];
	int ws_only;
	if (!path_within_workspace(path, resolved, sizeof(resolved))) {
		snprintf(result_buf, max_len, "{\"error\":\"path outside workspace\"}");
		return -1;
	}
	ws_only = g_file_cfg ? config_workspace_only(g_file_cfg) : 0;
	if (ws_only && realpath(path, resolved) == NULL) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot list directory\"}");
		return -1;
	}
	DIR *d = opendir(resolved);
	if (!d) {
		snprintf(result_buf, max_len, "{\"error\":\"cannot list directory\"}");
		return -1;
	}
	size_t used = 0;
	result_buf[0] = '\0';
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && count < LIST_ENTRIES_MAX) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		int n = snprintf(result_buf + used, max_len - used, "%s\n", e->d_name);
		if (n > 0 && (size_t)n < max_len - used) used += (size_t)n;
		count++;
	}
	closedir(d);
	if (used > 0 && result_buf[used - 1] == '\n') result_buf[used - 1] = '\0';
	return 0;
}

static int file_execute(const char *args_json, char *result_buf, size_t max_len)
{
	if (!args_json || !result_buf || max_len == 0) return -1;
	cJSON *root = cJSON_Parse(args_json);
	if (!root || !cJSON_IsObject(root)) {
		if (root) cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"invalid JSON\"}");
		return -1;
	}
	cJSON *op = cJSON_GetObjectItem(root, "operation");
	cJSON *path = cJSON_GetObjectItem(root, "path");
	if (!op || !cJSON_IsString(op) || !path || !cJSON_IsString(path)) {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"missing operation or path\"}");
		return -1;
	}
	const char *path_str = path->valuestring;
	cJSON *content = cJSON_GetObjectItem(root, "content");
	const char *content_str = (content && cJSON_IsString(content)) ? content->valuestring : "";
	int ret;
	if (strcmp(op->valuestring, "read_file") == 0)
		ret = file_read(path_str, result_buf, max_len);
	else if (strcmp(op->valuestring, "write_file") == 0)
		ret = file_write(path_str, content_str, result_buf, max_len);
	else if (strcmp(op->valuestring, "list_dir") == 0)
		ret = file_list(path_str, result_buf, max_len);
	else {
		cJSON_Delete(root);
		snprintf(result_buf, max_len, "{\"error\":\"invalid operation\"}");
		return -1;
	}
	cJSON_Delete(root);
	return ret;
}

static const tool_t FILE_TOOL = {
	.name = "file",
	.description = "Read, write, or list files. Paths must be within workspace when workspace_only is enabled.",
	.parameters_json = FILE_PARAMS,
	.execute = file_execute,
};

const tool_t *tool_file_get(void)
{
	return &FILE_TOOL;
}

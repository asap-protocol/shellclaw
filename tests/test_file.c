/**
 * @file test_file.c
 * @brief Unit tests for file tool: read, write, list_dir, workspace boundary.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "tools/tool.h"
#include "tools/file.h"
#include "core/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_failed = 0;

#define MU_ASSERT(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s\n", (msg)); \
		tests_failed++; \
		return; \
	} \
} while (0)

#define MU_RUN(test) do { test(); } while (0)

static void test_file_read_write_list(void)
{
	char cwd[PATH_MAX];
	MU_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/build/test_file_dir", cwd);
	char config_path[PATH_MAX];
	snprintf(config_path, sizeof(config_path), "%s/build/test_file_config.toml", cwd);
	/* Idempotent cleanup: remove leftovers from previous failed run */
	unlink(config_path);
	rmdir(dir);
	mkdir(dir, 0755);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/test.txt", dir);
	config_t *cfg = NULL;
	char errbuf[256];
	char config_content[512];
	snprintf(config_content, sizeof(config_content),
		"[agent]\nmodel=\"x\"\n[memory]\ndb_path=\"/tmp/db\"\n[sandbox]\nworkspace_only=true\nworkspace_path=\"%s\"\n",
		cwd);
	FILE *f = fopen(config_path, "w");
	MU_ASSERT(f != NULL, "create config file");
	fprintf(f, "%s", config_content);
	fclose(f);
	int load_ret = config_load(config_path, &cfg, errbuf, sizeof(errbuf));
	MU_ASSERT(load_ret == 0 && cfg != NULL, "load config");
	tool_file_set_config(cfg);
	const tool_t *t = tool_file_get();
	char buf[4096];
	char write_path[PATH_MAX];
	snprintf(write_path, sizeof(write_path), "%s/test.txt", dir);
	char args[1024];
	snprintf(args, sizeof(args), "{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"hello\"}", write_path);
	int r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == 0, "write_file succeeds");
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s\"}", write_path);
	r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == 0, "read_file succeeds");
	MU_ASSERT(strcmp(buf, "hello") == 0, "content matches");
	snprintf(args, sizeof(args), "{\"operation\":\"list_dir\",\"path\":\"%s\"}", dir);
	r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == 0, "list_dir succeeds");
	MU_ASSERT(strstr(buf, "test.txt") != NULL, "test.txt in listing");
	config_free(cfg);
	unlink(path);
	rmdir(dir);
	unlink(config_path);
}

static void test_file_empty_workspace_denies_all(void)
{
	config_t *cfg = NULL;
	char errbuf[256];
	char config_path[PATH_MAX];
	char cwd[PATH_MAX];
	MU_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
	snprintf(config_path, sizeof(config_path), "%s/build/test_file_empty_ws.toml", cwd);
	FILE *f = fopen(config_path, "w");
	MU_ASSERT(f != NULL, "create config");
	fprintf(f, "[agent]\nmodel=\"x\"\n[memory]\ndb_path=\"/tmp/db\"\n[sandbox]\nworkspace_only=true\nworkspace_path=\"\"\n");
	fclose(f);
	config_load(config_path, &cfg, errbuf, sizeof(errbuf));
	MU_ASSERT(cfg != NULL, "load config");
	tool_file_set_config(cfg);
	const tool_t *t = tool_file_get();
	char buf[256];
	int r = t->execute("{\"operation\":\"read_file\",\"path\":\"/etc/passwd\"}", buf, sizeof(buf));
	MU_ASSERT(r == -1 || strstr(buf, "outside") != NULL, "empty workspace denies all paths");
	config_free(cfg);
	unlink(config_path);
}

static void test_file_outside_workspace_rejected(void)
{
	char cwd[PATH_MAX];
	MU_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
	char config_path[PATH_MAX];
	snprintf(config_path, sizeof(config_path), "%s/build/test_file_ws_config.toml", cwd);
	config_t *cfg = NULL;
	char errbuf[256];
	FILE *f = fopen(config_path, "w");
	MU_ASSERT(f != NULL, "create config");
	fprintf(f, "[agent]\nmodel=\"x\"\n[memory]\ndb_path=\"/tmp/db\"\n[sandbox]\nworkspace_only=true\nworkspace_path=\"%s\"\n", cwd);
	fclose(f);
	config_load(config_path, &cfg, errbuf, sizeof(errbuf));
	MU_ASSERT(cfg != NULL, "load config");
	tool_file_set_config(cfg);
	const tool_t *t = tool_file_get();
	char buf[256];
	int r = t->execute("{\"operation\":\"read_file\",\"path\":\"/etc/passwd\"}", buf, sizeof(buf));
	MU_ASSERT(r == -1 || strstr(buf, "outside") != NULL, "outside workspace rejected");
	config_free(cfg);
	unlink(config_path);
}

/* Build a config with workspace_path=tmpdir; return allocated path; caller must config_free + unlink. */
static config_t *make_ws_config(const char *workspace, char *config_path_out, size_t path_cap)
{
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return NULL;
	snprintf(config_path_out, path_cap, "%s/build/test_file_ws2.toml", cwd);
	FILE *f = fopen(config_path_out, "w");
	if (!f) return NULL;
	fprintf(f, "[agent]\nmodel=\"x\"\n[memory]\ndb_path=\"/tmp/db\"\n[sandbox]\nworkspace_only=true\nworkspace_path=\"%s\"\n", workspace);
	fclose(f);
	config_t *cfg = NULL;
	config_load(config_path_out, &cfg, NULL, 0);
	return cfg;
}

static void test_path_traversal_rejected(void)
{
	char tmpdir[PATH_MAX];
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/sc_test_trav_%d", (int)getpid());
	if (mkdir(tmpdir, 0755) != 0 && errno != EEXIST) return;
	char config_path[PATH_MAX];
	config_t *cfg = make_ws_config(tmpdir, config_path, sizeof(config_path));
	MU_ASSERT(cfg != NULL, "traversal: load config");
	tool_file_set_config(cfg);
	const tool_t *t = tool_file_get();
	char args[PATH_MAX + 64];
	char buf[256];
	/* Classic ../ traversal attempting to reach /etc/passwd */
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s/../../../etc/passwd\"}", tmpdir);
	int r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == -1, "traversal path rejected");
	MU_ASSERT(strstr(buf, "outside") != NULL || strstr(buf, "error") != NULL,
		"traversal rejected with error");
	config_free(cfg);
	unlink(config_path);
	rmdir(tmpdir);
}

static void test_symlink_escape_rejected(void)
{
	char tmpdir[PATH_MAX];
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/sc_test_sym_%d", (int)getpid());
	if (mkdir(tmpdir, 0755) != 0 && errno != EEXIST) return;
	/* Create a symlink inside the workspace pointing outside */
	char symlink_path[PATH_MAX];
	snprintf(symlink_path, sizeof(symlink_path), "%s/escape_link", tmpdir);
	unlink(symlink_path);
	if (symlink("/etc", symlink_path) != 0) {
		rmdir(tmpdir);
		return; /* Skip if symlink creation fails */
	}
	char config_path[PATH_MAX];
	config_t *cfg = make_ws_config(tmpdir, config_path, sizeof(config_path));
	MU_ASSERT(cfg != NULL, "symlink: load config");
	tool_file_set_config(cfg);
	const tool_t *t = tool_file_get();
	char args[PATH_MAX + 64];
	char buf[256];
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s/escape_link/passwd\"}", tmpdir);
	int r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == -1, "symlink escape rejected");
	MU_ASSERT(strstr(buf, "outside") != NULL || strstr(buf, "error") != NULL,
		"symlink escape rejected with error");
	config_free(cfg);
	unlink(config_path);
	unlink(symlink_path);
	rmdir(tmpdir);
}

static int slurp_file(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f) return -1;
	n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = '\0';
	return 0;
}

static void test_write_does_not_truncate_file_used_as_directory(void)
{
	char tmpdir[PATH_MAX];
	char victim[PATH_MAX];
	char nested[PATH_MAX];
	char config_path[PATH_MAX];
	char args[PATH_MAX + 128];
	char buf[256];
	char kept[64];
	config_t *cfg;
	const tool_t *t;
	FILE *f;
	int r;

	snprintf(tmpdir, sizeof(tmpdir), "/tmp/sc_test_filedir_%d", (int)getpid());
	if (mkdir(tmpdir, 0755) != 0 && errno != EEXIST) return;
	snprintf(victim, sizeof(victim), "%s/important.md", tmpdir);
	f = fopen(victim, "w");
	MU_ASSERT(f != NULL, "create victim file");
	fputs("KEEP", f);
	fclose(f);
	cfg = make_ws_config(tmpdir, config_path, sizeof(config_path));
	MU_ASSERT(cfg != NULL, "file-as-dir: load config");
	tool_file_set_config(cfg);
	t = tool_file_get();
	snprintf(nested, sizeof(nested), "%s/important.md/nested.txt", tmpdir);
	snprintf(args, sizeof(args),
		"{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"PWNED\"}", nested);
	r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == -1, "write through file-as-directory is rejected");
	MU_ASSERT(slurp_file(victim, kept, sizeof(kept)) == 0, "victim still readable");
	MU_ASSERT(strcmp(kept, "KEEP") == 0, "victim content preserved");
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s\"}", nested);
	r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == -1, "read through file-as-directory is rejected");
	MU_ASSERT(strstr(buf, "KEEP") == NULL, "read does not leak victim contents");
	config_free(cfg);
	unlink(config_path);
	unlink(victim);
	rmdir(tmpdir);
}

static void test_write_does_not_collapse_missing_parent_onto_basename(void)
{
	char tmpdir[PATH_MAX];
	char victim[PATH_MAX];
	char nested[PATH_MAX];
	char config_path[PATH_MAX];
	char args[PATH_MAX + 128];
	char buf[256];
	char kept[64];
	config_t *cfg;
	const tool_t *t;
	FILE *f;
	int r;

	snprintf(tmpdir, sizeof(tmpdir), "/tmp/sc_test_missdir_%d", (int)getpid());
	if (mkdir(tmpdir, 0755) != 0 && errno != EEXIST) return;
	snprintf(victim, sizeof(victim), "%s/notes.md", tmpdir);
	f = fopen(victim, "w");
	MU_ASSERT(f != NULL, "create same-basename victim");
	fputs("KEEP", f);
	fclose(f);
	cfg = make_ws_config(tmpdir, config_path, sizeof(config_path));
	MU_ASSERT(cfg != NULL, "missing-parent: load config");
	tool_file_set_config(cfg);
	t = tool_file_get();
	snprintf(nested, sizeof(nested), "%s/missing_dir/notes.md", tmpdir);
	snprintf(args, sizeof(args),
		"{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"PWNED\"}", nested);
	r = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(r == -1, "write with missing parent is rejected");
	MU_ASSERT(slurp_file(victim, kept, sizeof(kept)) == 0, "workspace notes.md still readable");
	MU_ASSERT(strcmp(kept, "KEEP") == 0, "missing parent does not overwrite same basename");
	config_free(cfg);
	unlink(config_path);
	unlink(victim);
	rmdir(tmpdir);
}

int main(void)
{
	MU_RUN(test_file_read_write_list);
	MU_RUN(test_file_empty_workspace_denies_all);
	MU_RUN(test_file_outside_workspace_rejected);
	MU_RUN(test_path_traversal_rejected);
	MU_RUN(test_symlink_escape_rejected);
	MU_RUN(test_write_does_not_truncate_file_used_as_directory);
	MU_RUN(test_write_does_not_collapse_missing_parent_onto_basename);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}

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
#include <signal.h>
#include <sys/resource.h>
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

/*
 * file_write used fopen("w"), which truncates before the new bytes are
 * durable. ENOSPC/EFBIG then destroyed the live workspace file. Atomic
 * temp+rename must keep the original contents when the write fails.
 */
static void test_file_write_failure_preserves_existing(void)
{
	char tmpdir[PATH_MAX];
	char notes_path[PATH_MAX];
	char config_path[PATH_MAX];
	char args[PATH_MAX + 640];
	char buf[512];
	char oversized[512];
	struct rlimit old_lim;
	struct rlimit new_lim;
	config_t *cfg;
	const tool_t *t;
	int write_ret;
	size_t i;

	snprintf(tmpdir, sizeof(tmpdir), "/tmp/sc_test_fsize_%d", (int)getpid());
	if (mkdir(tmpdir, 0755) != 0 && errno != EEXIST) return;
	snprintf(notes_path, sizeof(notes_path), "%s/notes.md", tmpdir);
	cfg = make_ws_config(tmpdir, config_path, sizeof(config_path));
	MU_ASSERT(cfg != NULL, "fsize: load config");
	tool_file_set_config(cfg);
	t = tool_file_get();

	snprintf(args, sizeof(args),
		"{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"original notes that must survive\"}",
		notes_path);
	MU_ASSERT(t->execute(args, buf, sizeof(buf)) == 0, "seed original file");
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s\"}", notes_path);
	MU_ASSERT(t->execute(args, buf, sizeof(buf)) == 0, "read seeded file");
	MU_ASSERT(strstr(buf, "original notes that must survive") != NULL, "seeded content present");

	for (i = 0; i < sizeof(oversized) - 1; i++)
		oversized[i] = 'A';
	oversized[sizeof(oversized) - 1] = '\0';

	MU_ASSERT(getrlimit(RLIMIT_FSIZE, &old_lim) == 0, "getrlimit FSIZE");
	new_lim = old_lim;
	new_lim.rlim_cur = 8;
	(void)signal(SIGXFSZ, SIG_IGN);
	MU_ASSERT(setrlimit(RLIMIT_FSIZE, &new_lim) == 0, "setrlimit FSIZE");

	snprintf(args, sizeof(args),
		"{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"%s\"}",
		notes_path, oversized);
	write_ret = t->execute(args, buf, sizeof(buf));
	MU_ASSERT(setrlimit(RLIMIT_FSIZE, &old_lim) == 0, "restore rlimit");

	MU_ASSERT(write_ret != 0, "oversized write fails");
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s\"}", notes_path);
	MU_ASSERT(t->execute(args, buf, sizeof(buf)) == 0, "read after failed write");
	MU_ASSERT(strstr(buf, "original notes that must survive") != NULL,
		"failed write must not wipe original");

	snprintf(args, sizeof(args),
		"{\"operation\":\"write_file\",\"path\":\"%s\",\"content\":\"recovered after limit\"}",
		notes_path);
	MU_ASSERT(t->execute(args, buf, sizeof(buf)) == 0, "write recovers after limit");
	snprintf(args, sizeof(args), "{\"operation\":\"read_file\",\"path\":\"%s\"}", notes_path);
	MU_ASSERT(t->execute(args, buf, sizeof(buf)) == 0, "read recovered file");
	MU_ASSERT(strstr(buf, "recovered after limit") != NULL, "recovered content present");

	config_free(cfg);
	unlink(config_path);
	unlink(notes_path);
	snprintf(notes_path, sizeof(notes_path), "%s/notes.md.tmp", tmpdir);
	unlink(notes_path);
	rmdir(tmpdir);
}

int main(void)
{
	MU_RUN(test_file_read_write_list);
	MU_RUN(test_file_empty_workspace_denies_all);
	MU_RUN(test_file_outside_workspace_rejected);
	MU_RUN(test_path_traversal_rejected);
	MU_RUN(test_symlink_escape_rejected);
	MU_RUN(test_file_write_failure_preserves_existing);
	printf("%d tests run, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}

/**
 * @file test_skill.c
 * @brief Unit tests for skill loader: directory scan, .md concatenation, missing dir.
 */

#include "core/config.h"
#include "core/skill.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define RUN(t) do { int r = (t); if (r) return r; } while (0)

static const char *TMP_DIR = "/tmp/shellclaw_test_skills";
static const char *TMP_CONFIG = "/tmp/shellclaw_test_skill_config.toml";

static int write_minimal_config(const char *skills_dir)
{
	FILE *f = fopen(TMP_CONFIG, "w");
	if (!f) { fprintf(stderr, "FAIL: cannot write %s\n", TMP_CONFIG); return 1; }
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 5\n");
	fprintf(f, "[memory]\ndb_path = \"/tmp/shellclaw_skill_test.db\"\n");
	if (skills_dir)
		fprintf(f, "[skills]\ndir = \"%s\"\n", skills_dir);
	fclose(f);
	return 0;
}

static int write_config_with_soul_identity(const char *soul_path, const char *identity_path, const char *skills_dir)
{
	FILE *f = fopen(TMP_CONFIG, "w");
	if (!f) { fprintf(stderr, "FAIL: cannot write %s\n", TMP_CONFIG); return 1; }
	fprintf(f, "[agent]\nmodel = \"test\"\nmax_tool_iterations = 5\n");
	if (soul_path || identity_path) {
		fprintf(f, "[agent.identity]\n");
		if (soul_path) fprintf(f, "soul = \"%s\"\n", soul_path);
		if (identity_path) fprintf(f, "identity = \"%s\"\n", identity_path);
	}
	fprintf(f, "[memory]\ndb_path = \"/tmp/shellclaw_skill_test.db\"\n");
	if (skills_dir) fprintf(f, "[skills]\ndir = \"%s\"\n", skills_dir);
	fclose(f);
	return 0;
}

static int test_load_two_md_files(void)
{
	char cmd[256];
	char path[256];
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", TMP_DIR, TMP_DIR);
	ASSERT(system(cmd) == 0);
	snprintf(path, sizeof(path), "%s/a.md", TMP_DIR);
	FILE *a = fopen(path, "w");
	ASSERT(a);
	fprintf(a, "Skill A content");
	fclose(a);
	snprintf(path, sizeof(path), "%s/b.md", TMP_DIR);
	FILE *b = fopen(path, "w");
	ASSERT(b);
	fprintf(b, "Skill B content");
	fclose(b);
	ASSERT(write_minimal_config(TMP_DIR) == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	char out[2048];
	ASSERT(skill_load_all(cfg, out, sizeof(out)) == 0);
	ASSERT(strstr(out, "Skill A content") != NULL);
	ASSERT(strstr(out, "Skill B content") != NULL);
	ASSERT(strstr(out, "---") != NULL);
	config_free(cfg);
	remove(TMP_CONFIG);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", TMP_DIR);
	(void)system(cmd);
	return 0;
}

static int test_missing_dir_no_crash(void)
{
	ASSERT(write_minimal_config("/tmp/nonexistent_skills_dir_xyz_123") == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	char out[256];
	int ret = skill_load_all(cfg, out, sizeof(out));
	ASSERT(ret == 0);
	ASSERT(out[0] == '\0');
	config_free(cfg);
	remove(TMP_CONFIG);
	return 0;
}

static int test_null_config_returns_error(void)
{
	char out[256];
	ASSERT(skill_load_all(NULL, out, sizeof(out)) == -1);
	return 0;
}

static int test_empty_buffer_size(void)
{
	ASSERT(write_minimal_config(TMP_DIR) == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	char out[1];
	ASSERT(skill_load_all(cfg, out, 0) == -1);
	config_free(cfg);
	remove(TMP_CONFIG);
	return 0;
}

static int test_system_prompt_base_order(void)
{
	const char *base = "/tmp/shellclaw_test_prompt";
	char cmd[512];
	char path[512];
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s/skills\"", base, base);
	ASSERT(system(cmd) == 0);
	snprintf(path, sizeof(path), "%s/soul.md", base);
	FILE *soul = fopen(path, "w");
	ASSERT(soul);
	fprintf(soul, "SOUL_CONTENT");
	fclose(soul);
	snprintf(path, sizeof(path), "%s/identity.md", base);
	FILE *ident = fopen(path, "w");
	ASSERT(ident);
	fprintf(ident, "IDENTITY_CONTENT");
	fclose(ident);
	snprintf(path, sizeof(path), "%s/skills/extra.md", base);
	FILE *sk = fopen(path, "w");
	ASSERT(sk);
	fprintf(sk, "SKILL_CONTENT");
	fclose(sk);
	snprintf(path, sizeof(path), "%s/soul.md", base);
	char identity_path[256];
	char skills_path[256];
	snprintf(identity_path, sizeof(identity_path), "%s/identity.md", base);
	snprintf(skills_path, sizeof(skills_path), "%s/skills", base);
	ASSERT(write_config_with_soul_identity(path, identity_path, skills_path) == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	char skills_buf[2048];
	ASSERT(skill_load_all(cfg, skills_buf, sizeof(skills_buf)) == 0);
	ASSERT(strstr(skills_buf, "SKILL_CONTENT") != NULL);
	char prompt_buf[2048];
	ASSERT(skill_build_system_prompt_base(cfg, skills_buf, prompt_buf, sizeof(prompt_buf)) == 0);
	const char *soul_pos = strstr(prompt_buf, "SOUL_CONTENT");
	const char *ident_pos = strstr(prompt_buf, "IDENTITY_CONTENT");
	const char *skill_pos = strstr(prompt_buf, "SKILL_CONTENT");
	ASSERT(soul_pos != NULL);
	ASSERT(ident_pos != NULL);
	ASSERT(skill_pos != NULL);
	ASSERT(soul_pos < ident_pos);
	ASSERT(ident_pos < skill_pos);
	config_free(cfg);
	remove(TMP_CONFIG);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", base);
	(void)system(cmd);
	return 0;
}

static int test_skill_crud(void)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", TMP_DIR, TMP_DIR);
	ASSERT(system(cmd) == 0);
	ASSERT(write_minimal_config(TMP_DIR) == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(skill_create(cfg, "foo", "# Foo\nContent here") == 0);
	char *names[8];
	int n = skill_list_names(cfg, names, 8);
	ASSERT(n == 1);
	ASSERT(strcmp(names[0], "foo") == 0);
	free(names[0]);
	char content[256];
	ASSERT(skill_get_content(cfg, "foo", content, sizeof(content)) == 0);
	ASSERT(strstr(content, "Content here") != NULL);
	ASSERT(skill_update(cfg, "foo", "# Foo\nUpdated content") == 0);
	ASSERT(skill_get_content(cfg, "foo", content, sizeof(content)) == 0);
	ASSERT(strstr(content, "Updated content") != NULL);
	ASSERT(skill_delete(cfg, "foo") == 0);
	ASSERT(skill_get_content(cfg, "foo", content, sizeof(content)) == -1);
	n = skill_list_names(cfg, names, 8);
	ASSERT(n == 0);
	config_free(cfg);
	remove(TMP_CONFIG);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", TMP_DIR);
	(void)system(cmd);
	return 0;
}

/*
 * skill_update used fopen("w") which truncates before write. A failed write
 * (ENOSPC/EFBIG) wiped the live skill. Atomic temp+rename must preserve it.
 */
static int test_skill_update_write_failure_preserves_existing(void)
{
	char cmd[256];
	char content[256];
	char oversized[512];
	struct rlimit old_lim;
	struct rlimit new_lim;
	int update_ret;
	size_t i;
	config_t *cfg = NULL;
	char errbuf[256];

	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", TMP_DIR, TMP_DIR);
	ASSERT(system(cmd) == 0);
	ASSERT(write_minimal_config(TMP_DIR) == 0);
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(skill_create(cfg, "keepme", "# Keep\nOriginal skill body that must survive") == 0);
	ASSERT(skill_get_content(cfg, "keepme", content, sizeof(content)) == 0);
	ASSERT(strstr(content, "Original skill body that must survive") != NULL);

	for (i = 0; i < sizeof(oversized) - 1; i++)
		oversized[i] = 'A';
	oversized[sizeof(oversized) - 1] = '\0';

	ASSERT(getrlimit(RLIMIT_FSIZE, &old_lim) == 0);
	new_lim = old_lim;
	new_lim.rlim_cur = 8;
	(void)signal(SIGXFSZ, SIG_IGN);
	ASSERT(setrlimit(RLIMIT_FSIZE, &new_lim) == 0);

	update_ret = skill_update(cfg, "keepme", oversized);
	ASSERT(setrlimit(RLIMIT_FSIZE, &old_lim) == 0);

	ASSERT(update_ret != 0);
	ASSERT(skill_get_content(cfg, "keepme", content, sizeof(content)) == 0);
	ASSERT(strstr(content, "Original skill body that must survive") != NULL);

	ASSERT(skill_update(cfg, "keepme", "# Keep\nRecovered after limit") == 0);
	ASSERT(skill_get_content(cfg, "keepme", content, sizeof(content)) == 0);
	ASSERT(strstr(content, "Recovered after limit") != NULL);

	config_free(cfg);
	remove(TMP_CONFIG);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", TMP_DIR);
	(void)system(cmd);
	return 0;
}

static int test_hot_reload_watch(void)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" && mkdir -p \"%s\"", TMP_DIR, TMP_DIR);
	ASSERT(system(cmd) == 0);
	char path[256];
	snprintf(path, sizeof(path), "%s/watch_test.md", TMP_DIR);
	FILE *f = fopen(path, "w");
	ASSERT(f);
	fprintf(f, "Watch test content");
	fclose(f);
	ASSERT(write_minimal_config(TMP_DIR) == 0);
	config_t *cfg = NULL;
	char errbuf[256];
	ASSERT(config_load(TMP_CONFIG, &cfg, errbuf, sizeof(errbuf)) == 0);
	ASSERT(skill_watch_start(cfg, 0) == 0);
	char out[2048];
	ASSERT(skill_load_all(cfg, out, sizeof(out)) == 0);
	ASSERT(strstr(out, "Watch test content") != NULL);
	skill_watch_stop();
	ASSERT(skill_load_all(cfg, out, sizeof(out)) == 0);
	ASSERT(strstr(out, "Watch test content") != NULL);
	config_free(cfg);
	remove(TMP_CONFIG);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", TMP_DIR);
	(void)system(cmd);
	return 0;
}

int main(void)
{
	RUN(test_null_config_returns_error());
	RUN(test_empty_buffer_size());
	RUN(test_load_two_md_files());
	RUN(test_missing_dir_no_crash());
	RUN(test_system_prompt_base_order());
	RUN(test_skill_crud());
	RUN(test_skill_update_write_failure_preserves_existing());
	RUN(test_hot_reload_watch());
	printf("test_skill: all tests passed\n");
	return 0;
}

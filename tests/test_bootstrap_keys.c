/**
 * @file test_bootstrap_keys.c
 * @brief Ed25519 key permission checks (lazy load on signed manifest path).
 */
#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "asap/manifest_keys.h"
#include "crypto/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ASSERT(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, #c); \
		return 1; \
	} \
} while (0)

static const char *shellclaw_bin(void)
{
	const char *bin = getenv("SHELLCLAW_TEST_BIN");
	return (bin && bin[0]) ? bin : "build/shellclaw";
}

static int test_cli_starts_without_keys_dir(void)
{
	char home[128];
	char db_path[256];
	char cfg_path[256];
	char cmd[1024];
	FILE *fp;
	int status;

	ASSERT(test_runner_mkdtemp_path("shellclaw_bootstrap_nkeys", home, sizeof(home)) == 0);
	ASSERT(test_runner_mkstemp_path("shellclaw_bootstrap_db", db_path, sizeof(db_path)) == 0);
	remove(db_path);
	ASSERT(test_runner_mkstemp_path("shellclaw_bootstrap_cfg", cfg_path, sizeof(cfg_path)) == 0);
	{
		FILE *f = fopen(cfg_path, "w");
		ASSERT(f);
		fprintf(f,
			"[agent]\nmodel = \"bootstrap-keys-test\"\nmax_tool_iterations = 3\n\n"
			"[memory]\ndb_path = \"%s\"\n",
			db_path);
		fclose(f);
	}
	snprintf(cmd, sizeof(cmd),
		 "SHELLCLAW_HOME=\"%s\" \"%s\" --config \"%s\" -m \"bootstrap-keys-test\" 2>&1",
		 home, shellclaw_bin(), cfg_path);
	fp = popen(cmd, "r");
	ASSERT(fp != NULL);
	while (fgets(cmd, sizeof(cmd), fp) != NULL)
		;
	status = pclose(fp);
	if (status != -1 && WIFEXITED(status))
		status = WEXITSTATUS(status);
	else if (status == -1)
		status = 1;
	ASSERT(status == 0);
	remove(cfg_path);
	remove(db_path);
	{
		char rm_cmd[512];
		snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", home);
		(void)system(rm_cmd);
	}
	return 0;
}

static int test_ensure_loaded_rejects_loose_keys(void)
{
	char home[128];
	char keys_dir[256];
	char priv_path[512];
	char pub_path[512];
	char err[256];
	unsigned char priv_buf[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	unsigned char pub_buf[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	FILE *f;

	ASSERT(test_runner_mkdtemp_path("shellclaw_ensure_keys", home, sizeof(home)) == 0);
	snprintf(keys_dir, sizeof(keys_dir), "%s/keys", home);
	ASSERT(mkdir(keys_dir, 0700) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", keys_dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", keys_dir);
	memset(priv_buf, 0xab, sizeof(priv_buf));
	memset(pub_buf, 0xcd, sizeof(pub_buf));
	f = fopen(priv_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(priv_buf, 1, sizeof(priv_buf), f) == sizeof(priv_buf));
	fclose(f);
	ASSERT(chmod(priv_path, 0644) == 0);
	f = fopen(pub_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(pub_buf, 1, sizeof(pub_buf), f) == sizeof(pub_buf));
	fclose(f);
	setenv("SHELLCLAW_HOME", home, 1);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	ASSERT(manifest_keys_ensure_loaded(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "permissions") != NULL);
	manifest_keys_reset();
	{
		char rm_cmd[512];
		snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", home);
		(void)system(rm_cmd);
	}
	return 0;
}

int main(void)
{
	int r = 0;
	if (access(shellclaw_bin(), X_OK) != 0) {
		printf("test_bootstrap_keys: skipped (%s not executable)\n",
		       shellclaw_bin());
		return 0;
	}
	r |= test_cli_starts_without_keys_dir();
	r |= test_ensure_loaded_rejects_loose_keys();
	if (r == 0)
		printf("test_bootstrap_keys: all tests passed\n");
	return r;
}

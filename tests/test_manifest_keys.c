/**
 * @file test_manifest_keys.c
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "asap/manifest_keys.h"
#include "crypto/crypto.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t MANIFEST_KEYS_TEST_SEED[32] = {
	0x5aU, 0x11U, 0x51U, 0xa4U, 0x59U, 0xfaU, 0xeaU, 0xdeU,
	0x3dU, 0x24U, 0x71U, 0x15U, 0xf9U, 0x4aU, 0xedU, 0xaeU,
	0x42U, 0x31U, 0x81U, 0x24U, 0x09U, 0x5aU, 0xfaU, 0xbeU,
	0x4dU, 0x14U, 0x51U, 0xa5U, 0x59U, 0xfaU, 0xedU, 0xeeU
};

static const uint8_t MANIFEST_KEYS_ROTATE_SEED[32] = {
	0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
	0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU, 0x10U,
	0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
	0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU, 0x20U
};

/* Distinct seeds for the 7-rotation prune test so each keygen is unique. */
static const uint8_t MANIFEST_KEYS_PRUNE_SEEDS[7][32] = {
	{ 0x11U, 0x21U, 0x31U, 0x41U, 0x51U, 0x61U, 0x71U, 0x81U,
	  0x12U, 0x22U, 0x32U, 0x42U, 0x52U, 0x62U, 0x72U, 0x82U,
	  0x13U, 0x23U, 0x33U, 0x43U, 0x53U, 0x63U, 0x73U, 0x83U,
	  0x14U, 0x24U, 0x34U, 0x44U, 0x54U, 0x64U, 0x74U, 0x84U },
	{ 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
	  0x29U, 0x2aU, 0x2bU, 0x2cU, 0x2dU, 0x2eU, 0x2fU, 0x30U,
	  0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
	  0x39U, 0x3aU, 0x3bU, 0x3cU, 0x3dU, 0x3eU, 0x3fU, 0x40U },
	{ 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
	  0x39U, 0x3aU, 0x3bU, 0x3cU, 0x3dU, 0x3eU, 0x3fU, 0x30U,
	  0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
	  0x49U, 0x4aU, 0x4bU, 0x4cU, 0x4dU, 0x4eU, 0x4fU, 0x50U },
	{ 0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
	  0x49U, 0x4aU, 0x4bU, 0x4cU, 0x4dU, 0x4eU, 0x4fU, 0x50U,
	  0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
	  0x59U, 0x5aU, 0x5bU, 0x5cU, 0x5dU, 0x5eU, 0x5fU, 0x60U },
	{ 0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
	  0x59U, 0x5aU, 0x5bU, 0x5cU, 0x5dU, 0x5eU, 0x5fU, 0x60U,
	  0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U,
	  0x69U, 0x6aU, 0x6bU, 0x6cU, 0x6dU, 0x6eU, 0x6fU, 0x70U },
	{ 0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U,
	  0x69U, 0x6aU, 0x6bU, 0x6cU, 0x6dU, 0x6eU, 0x6fU, 0x70U,
	  0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U,
	  0x79U, 0x7aU, 0x7bU, 0x7cU, 0x7dU, 0x7eU, 0x7fU, 0x80U },
	{ 0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U,
	  0x79U, 0x7aU, 0x7bU, 0x7cU, 0x7dU, 0x7eU, 0x7fU, 0x80U,
	  0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U, 0x88U,
	  0x89U, 0x8aU, 0x8bU, 0x8cU, 0x8dU, 0x8eU, 0x8fU, 0x90U }
};

static int file_mode_is_0600(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	return (st.st_mode & 0777U) == 0600U;
}

static int test_manifest_keys_create_rolls_back_on_pub_failure(void)
{
	char dir[128];
	char priv_path[512];
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_pub_fail", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	manifest_keys_set_dir_for_test(dir);
	manifest_keys_test_set_fail_pub_write(1);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) != 0);
	ASSERT(access(priv_path, F_OK) != 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_test_set_fail_pub_write(0);
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

static int test_manifest_keys_first_run_creates(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_keys", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	crypto_test_clear_randombytes_seed();
	ASSERT(access(priv_path, F_OK) == 0);
	ASSERT(access(pub_path, F_OK) == 0);
	ASSERT(file_mode_is_0600(priv_path));
	ASSERT(file_mode_is_0600(pub_path));
	{
		FILE *f = fopen(priv_path, "rb");
		long sz;
		ASSERT(f != NULL);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fclose(f);
		ASSERT(sz == (long)CRYPTO_ED25519_PRIVATE_KEY_SIZE);
	}
	{
		FILE *f = fopen(pub_path, "rb");
		long sz;
		ASSERT(f != NULL);
		fseek(f, 0, SEEK_END);
		sz = ftell(f);
		fclose(f);
		ASSERT(sz == (long)CRYPTO_ED25519_PUBLIC_KEY_SIZE);
	}
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(priv_path, sizeof(priv_path), "rm -rf \"%s\"", dir);
	(void)system(priv_path);
	return 0;
}

static int test_manifest_keys_rejects_invalid_priv_size(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	char err[256];
	FILE *f;
	unsigned char buf[CRYPTO_ED25519_PUBLIC_KEY_SIZE];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_bad_priv", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	f = fopen(priv_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(buf, 1, 4, f) == 4);
	fclose(f);
	ASSERT(chmod(priv_path, 0600) == 0);
	f = fopen(pub_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
	fclose(f);
	manifest_keys_set_dir_for_test(dir);
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "read") != NULL);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_rejects_invalid_pub_size(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	char err[256];
	FILE *f;
	unsigned char buf[CRYPTO_ED25519_PRIVATE_KEY_SIZE];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_bad_pub", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	memset(buf, 0xcd, sizeof(buf));
	f = fopen(priv_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
	fclose(f);
	ASSERT(chmod(priv_path, 0600) == 0);
	f = fopen(pub_path, "wb");
	ASSERT(f);
	ASSERT(fwrite(buf, 1, 8, f) == 8);
	fclose(f);
	manifest_keys_set_dir_for_test(dir);
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "read") != NULL);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_rejects_loose_priv_perms(void)
{
	char dir[128];
	char priv_path[512];
	char err[256];
	FILE *f;
	unsigned char buf[CRYPTO_ED25519_PRIVATE_KEY_SIZE];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_keys3", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	memset(buf, 0xab, sizeof(buf));
	f = fopen(priv_path, "wb");
	ASSERT(f != NULL);
	ASSERT(fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
	fclose(f);
	ASSERT(chmod(priv_path, 0644) == 0);
	manifest_keys_set_dir_for_test(dir);
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "permissions") != NULL);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_rejects_overlong_home(void)
{
	char home[520];
	char cmd[576];

	memset(home, 'h', 507U);
	home[507U] = '\0';
	setenv("SHELLCLAW_HOME", home, 1);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	ASSERT(manifest_keys_load(NULL, 0) != 0);
	manifest_keys_reset();
	unsetenv("SHELLCLAW_HOME");
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", home);
	(void)system(cmd);
	return 0;
}

static int find_one_backup(const char *keys_dir, const char *prefix, char *out, size_t out_sz)
{
	DIR *d;
	struct dirent *ent;
	int found;

	found = 0;
	d = opendir(keys_dir);
	if (!d)
		return -1;
	while ((ent = readdir(d)) != NULL) {
		size_t plen;
		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;
		plen = strlen(prefix);
		if (ent->d_name[plen] == '\0' || strncmp(ent->d_name + plen, ".bak.", 5) != 0)
			continue;
		if (snprintf(out, out_sz, "%s/%s", keys_dir, ent->d_name) >= (int)out_sz)
			continue;
		found = 1;
		break;
	}
	closedir(d);
	return found ? 0 : -1;
}

static int count_backups(const char *keys_dir, const char *prefix)
{
	DIR *d;
	struct dirent *ent;
	int count;

	count = 0;
	d = opendir(keys_dir);
	if (!d)
		return -1;
	while ((ent = readdir(d)) != NULL) {
		size_t plen;
		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;
		plen = strlen(prefix);
		if (ent->d_name[plen] == '\0' ||
		    strncmp(ent->d_name + plen, ".bak.", 5) != 0)
			continue;
		count++;
	}
	closedir(d);
	return count;
}

static int read_live_keypair(const char *priv_path, const char *pub_path,
	uint8_t *priv_out, uint8_t *pub_out)
{
	FILE *f;

	f = fopen(priv_path, "rb");
	if (!f)
		return -1;
	if (fread(priv_out, 1, CRYPTO_ED25519_PRIVATE_KEY_SIZE, f) !=
	    CRYPTO_ED25519_PRIVATE_KEY_SIZE) {
		fclose(f);
		return -1;
	}
	fclose(f);
	f = fopen(pub_path, "rb");
	if (!f)
		return -1;
	if (fread(pub_out, 1, CRYPTO_ED25519_PUBLIC_KEY_SIZE, f) !=
	    CRYPTO_ED25519_PUBLIC_KEY_SIZE) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int test_manifest_keys_rotate_atomic(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	char bak_pub_path[576];
	uint8_t pub_before[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t pub_after[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t pub_bak[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	char err[256];
	FILE *f;
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_rotate", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	f = fopen(pub_path, "rb");
	ASSERT(f != NULL);
	ASSERT(fread(pub_before, 1, sizeof(pub_before), f) == sizeof(pub_before));
	fclose(f);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_ROTATE_SEED);
	manifest_keys_reset();
	ASSERT(manifest_keys_rotate(err, sizeof(err)) == 0);
	f = fopen(pub_path, "rb");
	ASSERT(f != NULL);
	ASSERT(fread(pub_after, 1, sizeof(pub_after), f) == sizeof(pub_after));
	fclose(f);
	ASSERT(memcmp(pub_before, pub_after, sizeof(pub_before)) != 0);
	ASSERT(find_one_backup(dir, "ed25519.pub", bak_pub_path, sizeof(bak_pub_path)) == 0);
	f = fopen(bak_pub_path, "rb");
	ASSERT(f != NULL);
	ASSERT(fread(pub_bak, 1, sizeof(pub_bak), f) == sizeof(pub_bak));
	fclose(f);
	ASSERT(memcmp(pub_bak, pub_before, sizeof(pub_before)) == 0);
	ASSERT(file_mode_is_0600(priv_path));
	ASSERT(file_mode_is_0600(pub_path));
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

static int test_manifest_keys_rotate_backup_fail_preserves_live(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	uint8_t priv_before[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_before[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t priv_after[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_after[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	char err[256];
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_rotate_bakfail", dir,
			sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	ASSERT(read_live_keypair(priv_path, pub_path, priv_before, pub_before) == 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_test_set_fail_backup_write(1);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_ROTATE_SEED);
	ASSERT(manifest_keys_rotate(err, sizeof(err)) != 0);
	crypto_test_clear_randombytes_seed();
	ASSERT(read_live_keypair(priv_path, pub_path, priv_after, pub_after) == 0);
	ASSERT(memcmp(priv_before, priv_after, sizeof(priv_before)) == 0);
	ASSERT(memcmp(pub_before, pub_after, sizeof(pub_before)) == 0);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

static int test_manifest_keys_rotate_fails_preserves_keys(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	uint8_t priv_before[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_before[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t priv_after[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_after[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	char err[256];
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_rotate_fail", dir,
			sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	ASSERT(read_live_keypair(priv_path, pub_path, priv_before, pub_before) == 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();

	ASSERT(chmod(dir, 0555) == 0);
	ASSERT(manifest_keys_rotate(err, sizeof(err)) != 0);
	ASSERT(chmod(dir, 0755) == 0);
	ASSERT(read_live_keypair(priv_path, pub_path, priv_after, pub_after) == 0);
	ASSERT(memcmp(priv_before, priv_after, sizeof(priv_before)) == 0);
	ASSERT(memcmp(pub_before, pub_after, sizeof(pub_before)) == 0);

	manifest_keys_reset();
	manifest_keys_test_set_fail_pub_write(1);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_ROTATE_SEED);
	ASSERT(manifest_keys_rotate(err, sizeof(err)) != 0);
	crypto_test_clear_randombytes_seed();
	ASSERT(read_live_keypair(priv_path, pub_path, priv_after, pub_after) == 0);
	ASSERT(memcmp(priv_before, priv_after, sizeof(priv_before)) == 0);
	ASSERT(memcmp(pub_before, pub_after, sizeof(pub_before)) == 0);

	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

static int test_manifest_keys_rotate_rejects_loose_priv(void)
{
	char dir[128];
	char priv_path[512];
	char err[256];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_rotate_perm", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	crypto_test_clear_randombytes_seed();
	ASSERT(chmod(priv_path, 0644) == 0);
	manifest_keys_reset();
	ASSERT(manifest_keys_rotate(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "permissions") != NULL);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_second_run_reuses(void)
{
	char dir[128];
	uint8_t pub_disk[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	const uint8_t *pub_mem;
	FILE *f;

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_keys2", dir, sizeof(dir)) == 0);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	pub_mem = manifest_keys_public();
	ASSERT(pub_mem != NULL);
	{
		char pub_path[512];
		snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
		f = fopen(pub_path, "rb");
		ASSERT(f != NULL);
		ASSERT(fread(pub_disk, 1, sizeof(pub_disk), f) == sizeof(pub_disk));
		fclose(f);
		ASSERT(memcmp(pub_disk, pub_mem, sizeof(pub_disk)) == 0);
	}
	manifest_keys_reset();
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	ASSERT(memcmp(manifest_keys_public(), pub_disk, sizeof(pub_disk)) == 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_uses_shellclaw_home(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	struct stat st;
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_home", dir, sizeof(dir)) == 0);
	setenv("SHELLCLAW_HOME", dir, 1);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/keys/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/keys/ed25519.pub", dir);
	ASSERT(stat(priv_path, &st) == 0);
	ASSERT(stat(pub_path, &st) == 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	unsetenv("SHELLCLAW_HOME");
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

/* 5.4 TOCTOU: a symlinked ed25519.priv must be rejected even when the
 * attacker-controlled target is itself 0600 AND its embedded pub matches the
 * pub file (so the M1 consistency check would pass). Only the lstat perm check
 * (sees S_ISLNK, not S_ISREG) and O_NOFOLLOW at read time (ELOOP) reject it.
 * Under the old code (stat + open(O_RDONLY)) this target passed perms, the open
 * followed the link, and the M1 check passed — loading attacker-controlled key
 * bytes — so this is a true regression guard for the TOCTOU fix. */
static int test_manifest_keys_rejects_symlinked_priv(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	char target[256];
	char err[256];
	unsigned char target_buf[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	unsigned char pub_buf[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	FILE *f;

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_symlink", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	snprintf(target, sizeof(target), "/tmp/sc_symlink_target_%d", (int)getpid());
	/* seed(0xab..) || pub(0xcd..): embedded pub matches the pub file so the M1
	 * consistency check does not reject this on its own. */
	memset(target_buf, 0xab, sizeof(target_buf));
	memset(pub_buf, 0xcd, sizeof(pub_buf));
	memcpy(target_buf + CRYPTO_ED25519_PUBLIC_KEY_SIZE, pub_buf, sizeof(pub_buf));
	f = fopen(target, "wb");
	ASSERT(f != NULL);
	ASSERT(fwrite(target_buf, 1, sizeof(target_buf), f) == sizeof(target_buf));
	fclose(f);
	/* 0600 target passes the old stat perm check, isolating the symlink
	 * rejection as the defense under test. */
	ASSERT(chmod(target, 0600) == 0);
	ASSERT(symlink(target, priv_path) == 0);
	f = fopen(pub_path, "wb");
	ASSERT(f != NULL);
	ASSERT(fwrite(pub_buf, 1, sizeof(pub_buf), f) == sizeof(pub_buf));
	fclose(f);
	ASSERT(chmod(pub_path, 0600) == 0);
	manifest_keys_set_dir_for_test(dir);
	manifest_keys_reset();
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	unlink(priv_path);
	unlink(pub_path);
	unlink(target);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

static int test_manifest_keys_rejects_symlinked_tmp_on_write(void)
{
	char dir[128];
	char tmp_path[512];
	char target[256];
	char err[256];
	FILE *f;

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_wrsyl", dir, sizeof(dir)) == 0);
	snprintf(tmp_path, sizeof(tmp_path), "%s/ed25519.priv.tmp", dir);
	snprintf(target, sizeof(target), "/tmp/sc_wr_symlink_target_%d", (int)getpid());
	f = fopen(target, "wb");
	ASSERT(f != NULL);
	fclose(f);
	ASSERT(symlink(target, tmp_path) == 0);
	manifest_keys_set_dir_for_test(dir);
	manifest_keys_reset();
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	unlink(tmp_path);
	unlink(target);
	{
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
		(void)system(cmd);
	}
	return 0;
}

/* 5.6.1 M1: when the new pub write fails after the new priv landed, rotation
 * restores old_priv to disk and resets in-memory state to the OLD pair so
 * memory matches the on-disk keys (pub == priv[32..64]) and ensure_loaded is
 * idempotent. */
static int test_manifest_keys_rotate_restores_pub_consistency(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	uint8_t priv_before[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_before[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	const uint8_t *pub_mem;
	const uint8_t *priv_mem;
	char err[256];
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_rotate_pubcons", dir,
			sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	ASSERT(read_live_keypair(priv_path, pub_path, priv_before, pub_before) == 0);
	crypto_test_clear_randombytes_seed();
	manifest_keys_reset();
	manifest_keys_test_set_fail_pub_write(1);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_ROTATE_SEED);
	ASSERT(manifest_keys_rotate(err, sizeof(err)) != 0);
	crypto_test_clear_randombytes_seed();
	pub_mem = manifest_keys_public();
	priv_mem = manifest_keys_private();
	ASSERT(pub_mem != NULL);
	ASSERT(priv_mem != NULL);
	ASSERT(memcmp(pub_mem, pub_before, sizeof(pub_before)) == 0);
	ASSERT(memcmp(priv_mem, priv_before, sizeof(priv_before)) == 0);
	ASSERT(memcmp(pub_mem, priv_mem + CRYPTO_ED25519_PUBLIC_KEY_SIZE,
		    CRYPTO_ED25519_PUBLIC_KEY_SIZE) == 0);
	ASSERT(manifest_keys_ensure_loaded(err, sizeof(err)) == 0);
	ASSERT(memcmp(manifest_keys_public(), pub_before, sizeof(pub_before)) == 0);
	ASSERT(memcmp(manifest_keys_private(), priv_before, sizeof(priv_before)) == 0);
	manifest_keys_reset();
	manifest_keys_test_set_fail_pub_write(0);
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

/* 5.6.1 M1: a keys dir with a valid priv but a pub that does not match the
 * priv's embedded pub must be rejected at load with a mismatch error. */
static int test_manifest_keys_load_rejects_mismatched_pub(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	uint8_t priv_buf[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t pub_buf[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	uint8_t bad_pub[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	char err[256];
	FILE *f;
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_pub_mismatch", dir,
			sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(crypto_ed25519_keypair(pub_buf, priv_buf) == 0);
	crypto_test_clear_randombytes_seed();
	f = fopen(priv_path, "wb");
	ASSERT(f != NULL);
	ASSERT(fwrite(priv_buf, 1, sizeof(priv_buf), f) == sizeof(priv_buf));
	fclose(f);
	ASSERT(chmod(priv_path, 0600) == 0);
	memset(bad_pub, 0xcd, sizeof(bad_pub));
	ASSERT(memcmp(bad_pub, pub_buf, sizeof(pub_buf)) != 0);
	f = fopen(pub_path, "wb");
	ASSERT(f != NULL);
	ASSERT(fwrite(bad_pub, 1, sizeof(bad_pub), f) == sizeof(bad_pub));
	fclose(f);
	ASSERT(chmod(pub_path, 0600) == 0);
	manifest_keys_set_dir_for_test(dir);
	manifest_keys_reset();
	ASSERT(manifest_keys_load(err, sizeof(err)) != 0);
	ASSERT(strstr(err, "does not match") != NULL);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}

/* 5.6.2 M2: after several rotations, .bak.* files are pruned to the N
 * most-recent. MANIFEST_KEY_BACKUPS_KEEP is private to manifest_keys.c (== 5);
 * the count mirrored here must stay in sync with that constant. Distinct
 * per-second backup timestamps are forced with sleep(1) so pruning is actually
 * exercised (same-second rotates would overwrite one .bak.<ts> and mask the
 * prune logic). */
static int test_manifest_keys_rotate_prunes_backups(void)
{
	char dir[128];
	char priv_path[512];
	char pub_path[512];
	const int keep = 5;
	int i;
	char cmd[512];

	ASSERT(test_runner_mkdtemp_path("shellclaw_manifest_prune", dir, sizeof(dir)) == 0);
	snprintf(priv_path, sizeof(priv_path), "%s/ed25519.priv", dir);
	snprintf(pub_path, sizeof(pub_path), "%s/ed25519.pub", dir);
	manifest_keys_set_dir_for_test(dir);
	crypto_test_set_randombytes_seed(MANIFEST_KEYS_TEST_SEED);
	ASSERT(manifest_keys_load(NULL, 0) == 0);
	crypto_test_clear_randombytes_seed();
	for (i = 0; i < 7; i++) {
		manifest_keys_reset();
		crypto_test_set_randombytes_seed(MANIFEST_KEYS_PRUNE_SEEDS[i]);
		ASSERT(manifest_keys_rotate(NULL, 0) == 0);
		crypto_test_clear_randombytes_seed();
		/* Distinct second-resolution timestamps so each rotate makes a new
		 * .bak.<ts> file instead of overwriting the prior one. */
		if (i < 6)
			sleep(1);
	}
	ASSERT(count_backups(dir, "ed25519.priv") == keep);
	ASSERT(count_backups(dir, "ed25519.pub") == keep);
	manifest_keys_reset();
	manifest_keys_set_dir_for_test(NULL);
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
	(void)system(cmd);
	return 0;
}


int main(int argc, char **argv)
{
	int failed = 0;
	(void)argc;
	(void)argv;
	if (test_manifest_keys_create_rolls_back_on_pub_failure() != 0) {
		fprintf(stderr, "test_manifest_keys_create_rolls_back_on_pub_failure failed\n");
		failed++;
	}
	if (test_manifest_keys_first_run_creates() != 0) {
		fprintf(stderr, "test_manifest_keys_first_run_creates failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_invalid_priv_size() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_invalid_priv_size failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_invalid_pub_size() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_invalid_pub_size failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_loose_priv_perms() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_loose_priv_perms failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_symlinked_priv() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_symlinked_priv failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_symlinked_tmp_on_write() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_symlinked_tmp_on_write failed\n");
		failed++;
	}
	if (test_manifest_keys_rejects_overlong_home() != 0) {
		fprintf(stderr, "test_manifest_keys_rejects_overlong_home failed\n");
		failed++;
	}
	if (test_manifest_keys_load_rejects_mismatched_pub() != 0) {
		fprintf(stderr, "test_manifest_keys_load_rejects_mismatched_pub failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_atomic() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_atomic failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_backup_fail_preserves_live() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_backup_fail_preserves_live failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_fails_preserves_keys() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_fails_preserves_keys failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_rejects_loose_priv() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_rejects_loose_priv failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_restores_pub_consistency() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_restores_pub_consistency failed\n");
		failed++;
	}
	if (test_manifest_keys_rotate_prunes_backups() != 0) {
		fprintf(stderr, "test_manifest_keys_rotate_prunes_backups failed\n");
		failed++;
	}
	if (test_manifest_keys_second_run_reuses() != 0) {
		fprintf(stderr, "test_manifest_keys_second_run_reuses failed\n");
		failed++;
	}
	if (test_manifest_keys_uses_shellclaw_home() != 0) {
		fprintf(stderr, "test_manifest_keys_uses_shellclaw_home failed\n");
		failed++;
	}
	if (failed == 0)
		printf("test_manifest_keys: all tests passed\n");
	return failed;
}

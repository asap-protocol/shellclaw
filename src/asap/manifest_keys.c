/**
 * @file manifest_keys.c
 * @brief Ed25519 key load, persist, and rotation for ASAP signed manifests.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest_keys.h"
#include "core/config.h"
#include "crypto/crypto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MANIFEST_KEYS_SUBDIR "keys"
#define MANIFEST_PRIV_FILENAME "ed25519.priv"
#define MANIFEST_PUB_FILENAME "ed25519.pub"
#define MANIFEST_KEY_FILE_MODE 0600
/* Number of most-recent .bak.<ts> backups kept per key after rotation. */
#define MANIFEST_KEY_BACKUPS_KEEP 5

static uint8_t g_manifest_pubkey[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
static uint8_t g_manifest_privkey[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
static int g_manifest_keys_loaded;
static const char *g_manifest_keys_dir_test;
static int g_manifest_keys_test_fail_pub_write;
static int g_manifest_keys_test_fail_backup_write;

void manifest_keys_test_set_fail_pub_write(int enabled)
{
	g_manifest_keys_test_fail_pub_write = enabled ? 1 : 0;
}

void manifest_keys_test_set_fail_backup_write(int enabled)
{
	g_manifest_keys_test_fail_backup_write = enabled ? 1 : 0;
}

void manifest_keys_set_dir_for_test(const char *keys_dir)
{
	g_manifest_keys_dir_test = keys_dir;
}

static void manifest_keys_clear_loaded(void)
{
	g_manifest_keys_loaded = 0;
	memset(g_manifest_pubkey, 0, sizeof(g_manifest_pubkey));
	memset(g_manifest_privkey, 0, sizeof(g_manifest_privkey));
}

void manifest_keys_reset(void)
{
	manifest_keys_clear_loaded();
	g_manifest_keys_test_fail_pub_write = 0;
	g_manifest_keys_test_fail_backup_write = 0;
}

const uint8_t *manifest_keys_public(void)
{
	return g_manifest_keys_loaded ? g_manifest_pubkey : NULL;
}

const uint8_t *manifest_keys_private(void)
{
	return g_manifest_keys_loaded ? g_manifest_privkey : NULL;
}

static char *manifest_resolve_home_dir(void)
{
	const char *env = getenv("SHELLCLAW_HOME");

	if (env && env[0] != '\0')
		return strdup(env);
	return config_expand_tilde("~/.shellclaw");
}

static int manifest_keys_build_paths(char *priv_path, size_t priv_sz,
	char *pub_path, size_t pub_sz)
{
	const char *keys_dir;
	char *home = NULL;
	char keys_buf[512];

	if (g_manifest_keys_dir_test && g_manifest_keys_dir_test[0] != '\0') {
		keys_dir = g_manifest_keys_dir_test;
	} else {
		home = manifest_resolve_home_dir();
		if (!home)
			return -1;
		if (snprintf(keys_buf, sizeof(keys_buf), "%s/%s", home,
				MANIFEST_KEYS_SUBDIR) >= (int)sizeof(keys_buf)) {
			free(home);
			return -1;
		}
		keys_dir = keys_buf;
	}
	if (snprintf(priv_path, priv_sz, "%s/%s", keys_dir, MANIFEST_PRIV_FILENAME) >= (int)priv_sz ||
	    snprintf(pub_path, pub_sz, "%s/%s", keys_dir, MANIFEST_PUB_FILENAME) >= (int)pub_sz) {
		free(home);
		return -1;
	}
	free(home);
	return 0;
}

static int manifest_priv_permissions_ok(const char *priv_path)
{
	struct stat st;

	/* lstat, not stat: a symlink is S_ISLNK (not S_ISREG) here, so a symlinked
	 * priv key whose target may be permissive is rejected by the S_ISREG check
	 * on the link itself rather than following it to the target. */
	if (lstat(priv_path, &st) != 0)
		return -1;
	if (!S_ISREG(st.st_mode))
		return -1;
	if ((st.st_mode & 0077U) != 0U)
		return -1;
	return 0;
}

static int manifest_build_tmp_path(const char *final_path, char *tmp_path, size_t tmp_sz)
{
	int n;
	if (!final_path || !tmp_path || tmp_sz == 0U)
		return -1;
	n = snprintf(tmp_path, tmp_sz, "%s.tmp", final_path);
	return (n < 0 || (size_t)n >= tmp_sz) ? -1 : 0;
}

static int manifest_write_key_file(const char *path, const uint8_t *data, size_t len)
{
	int fd;
	size_t off = 0;
	struct stat st;

	if (g_manifest_keys_test_fail_backup_write && path != NULL &&
	    strstr(path, ".bak.") != NULL) {
		g_manifest_keys_test_fail_backup_write = 0;
		return -1;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, MANIFEST_KEY_FILE_MODE);
	if (fd < 0)
		return -1;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n <= 0) {
			close(fd);
			unlink(path);
			return -1;
		}
		off += (size_t)n;
	}
	if (fchmod(fd, MANIFEST_KEY_FILE_MODE) != 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	if (fsync(fd) != 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);
	return 0;
}

static int manifest_write_key_file_atomic(const char *final_path, const uint8_t *data, size_t len)
{
	char tmp_path[576];

	if (g_manifest_keys_test_fail_pub_write) {
		size_t flen = final_path ? strlen(final_path) : 0U;
		if (flen >= strlen(MANIFEST_PUB_FILENAME) &&
		    strcmp(final_path + flen - strlen(MANIFEST_PUB_FILENAME),
			MANIFEST_PUB_FILENAME) == 0) {
			g_manifest_keys_test_fail_pub_write = 0;
			return -1;
		}
	}
	if (manifest_build_tmp_path(final_path, tmp_path, sizeof(tmp_path)) != 0)
		return -1;
	if (manifest_write_key_file(tmp_path, data, len) != 0) {
		unlink(tmp_path);
		return -1;
	}
	if (rename(tmp_path, final_path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

static int manifest_read_key_file(const char *path, uint8_t *buf, size_t len)
{
	int fd;
	size_t off = 0;
	struct stat st;

	/* O_NOFOLLOW rejects symlinks at open time (fails with ELOOP), and fstat on
	 * the fd closes the stat(path)->open(path) TOCTOU: the regular-file and size
	 * checks now apply to the exact inode we read. */
	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    (size_t)st.st_size != len) {
		close(fd);
		return -1;
	}
	while (off < len) {
		ssize_t n = read(fd, buf + off, len - off);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)n;
	}
	close(fd);
	return 0;
}

static int manifest_keys_ensure_dir(const char *priv_path)
{
	char dir[512];
	const char *slash;
	size_t len;

	slash = strrchr(priv_path, '/');
	if (!slash || slash == priv_path)
		return -1;
	len = (size_t)(slash - priv_path);
	if (len >= sizeof(dir))
		return -1;
	memcpy(dir, priv_path, len);
	dir[len] = '\0';
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int manifest_keys_create_and_persist(const char *priv_path, const char *pub_path)
{
	if (manifest_keys_ensure_dir(priv_path) != 0)
		return -1;
	if (crypto_ed25519_keypair(g_manifest_pubkey, g_manifest_privkey) != 0)
		return -1;
	if (manifest_write_key_file_atomic(priv_path, g_manifest_privkey,
			CRYPTO_ED25519_PRIVATE_KEY_SIZE) != 0)
		return -1;
	if (manifest_write_key_file_atomic(pub_path, g_manifest_pubkey,
			CRYPTO_ED25519_PUBLIC_KEY_SIZE) != 0) {
		unlink(priv_path);
		return -1;
	}
	return 0;
}

struct manifest_bak_entry {
	char path[576];
	long long ts;
};

/* Parse the ".bak.<digits>" suffix of a backup filename into its timestamp.
 * @prefix is the base key filename (e.g. "ed25519.priv"); a matching name is
 * "<prefix>.bak.<digits>". Non-numeric suffixes (or no suffix) return -1 so
 * callers skip them rather than crash. */
static int manifest_keys_parse_bak_ts(const char *name, const char *prefix,
	char *out_path, size_t out_sz, long long *ts_out)
{
	size_t plen;
	const char *suffix;
	long long ts;
	char *end;

	plen = strlen(prefix);
	if (strncmp(name, prefix, plen) != 0)
		return -1;
	suffix = name + plen;
	if (strncmp(suffix, ".bak.", 5) != 0 || suffix[5] == '\0')
		return -1;
	errno = 0;
	ts = strtoll(suffix + 5, &end, 10);
	if (errno != 0 || *end != '\0' || end == suffix + 5)
		return -1;
	if (snprintf(out_path, out_sz, "%s", name) >= (int)out_sz)
		return -1;
	*ts_out = ts;
	return 0;
}

/* Collect .bak.* entries for one key prefix (priv or pub), sorted by timestamp
 * descending so the newest stay first. Returns the count found (<= cap). */
static int manifest_keys_collect_baks(const char *keys_dir, const char *prefix,
	struct manifest_bak_entry *out, int cap)
{
	DIR *d;
	struct dirent *ent;
	int count;

	count = 0;
	d = opendir(keys_dir);
	if (!d)
		return 0;
	while ((ent = readdir(d)) != NULL && count < cap) {
		struct manifest_bak_entry e;
		long long ts;
		int i;

		if (manifest_keys_parse_bak_ts(ent->d_name, prefix, e.path,
			    sizeof(e.path), &ts) != 0)
			continue;
		e.ts = ts;
		/* insertion sort descending: keep newest first. */
		for (i = count; i > 0 && out[i - 1].ts < e.ts; i--)
			out[i] = out[i - 1];
		out[i] = e;
		count++;
	}
	closedir(d);
	return count;
}

/* Unlink .bak.* entries for one prefix beyond the N most-recent (already sorted
 * descending by manifest_keys_collect_baks). */
static void manifest_keys_prune_prefix(const char *keys_dir,
	const struct manifest_bak_entry *entries, int count)
{
	int i;
	char full[576];

	for (i = MANIFEST_KEY_BACKUPS_KEEP; i < count; i++) {
		if (snprintf(full, sizeof(full), "%s/%s", keys_dir,
			    entries[i].path) >= (int)sizeof(full))
			continue;
		(void)unlink(full);
	}
}

/* Keep only the N most-recent .bak.<ts> backups per key, oldest first pruned.
 * Called on the rotation success path so .bak.* growth is bounded on edge HW. */
static void manifest_keys_prune_backups(const char *keys_dir)
{
	struct manifest_bak_entry priv_baks[64];
	struct manifest_bak_entry pub_baks[64];
	int priv_n;
	int pub_n;

	priv_n = manifest_keys_collect_baks(keys_dir, MANIFEST_PRIV_FILENAME,
		priv_baks, (int)(sizeof(priv_baks) / sizeof(priv_baks[0])));
	manifest_keys_prune_prefix(keys_dir, priv_baks, priv_n);
	pub_n = manifest_keys_collect_baks(keys_dir, MANIFEST_PUB_FILENAME,
		pub_baks, (int)(sizeof(pub_baks) / sizeof(pub_baks[0])));
	manifest_keys_prune_prefix(keys_dir, pub_baks, pub_n);
}

static void manifest_keys_set_error(char *err, size_t err_len, const char *msg)
{
	if (!err || err_len == 0U)
		return;
	snprintf(err, err_len, "%s", msg);
}

/* Ed25519 secret key is seed(32)||pub(32); the embedded pub must match the
 * standalone pub file, otherwise the on-disk keypair is inconsistent and must
 * not be used to sign. */
static int manifest_keys_pub_matches_priv(char *err, size_t err_len)
{
	if (memcmp(g_manifest_pubkey, g_manifest_privkey + CRYPTO_ED25519_PUBLIC_KEY_SIZE,
		    CRYPTO_ED25519_PUBLIC_KEY_SIZE) != 0) {
		manifest_keys_set_error(err, err_len,
			"ed25519.pub does not match ed25519.priv");
		return -1;
	}
	return 0;
}

static int manifest_keys_load_from_disk(const char *priv_path, const char *pub_path,
	char *err, size_t err_len)
{
	if (access(priv_path, F_OK) == 0 && manifest_priv_permissions_ok(priv_path) != 0) {
		manifest_keys_set_error(err, err_len,
			"ed25519.priv permissions too open (expected 0600)");
		return -1;
	}
	if (manifest_read_key_file(priv_path, g_manifest_privkey,
			CRYPTO_ED25519_PRIVATE_KEY_SIZE) != 0) {
		manifest_keys_set_error(err, err_len, "failed to read ed25519.priv");
		return -1;
	}
	if (manifest_read_key_file(pub_path, g_manifest_pubkey,
			CRYPTO_ED25519_PUBLIC_KEY_SIZE) != 0) {
		manifest_keys_set_error(err, err_len, "failed to read ed25519.pub");
		return -1;
	}
	if (manifest_keys_pub_matches_priv(err, err_len) != 0)
		return -1;
	return 0;
}

int manifest_keys_ensure_loaded(char *err, size_t err_len)
{
	if (g_manifest_keys_loaded) {
		char priv_path[512];
		char pub_path[512];

		if (manifest_keys_build_paths(priv_path, sizeof(priv_path),
				pub_path, sizeof(pub_path)) != 0) {
			manifest_keys_set_error(err, err_len, "failed to resolve keys directory");
			return -1;
		}
		if (access(priv_path, F_OK) == 0 && manifest_priv_permissions_ok(priv_path) != 0) {
			manifest_keys_clear_loaded();
			manifest_keys_set_error(err, err_len,
				"ed25519.priv permissions too open (expected 0600)");
			return -1;
		}
		return 0;
	}
	return manifest_keys_load(err, err_len);
}

int manifest_keys_load(char *err, size_t err_len)
{
	char priv_path[512];
	char pub_path[512];

	if (g_manifest_keys_loaded)
		return 0;
	if (manifest_keys_build_paths(priv_path, sizeof(priv_path),
			pub_path, sizeof(pub_path)) != 0) {
		manifest_keys_set_error(err, err_len, "failed to resolve keys directory");
		return -1;
	}
	if (access(priv_path, F_OK) == 0) {
		if (manifest_keys_load_from_disk(priv_path, pub_path, err, err_len) != 0)
			return -1;
	} else {
		if (manifest_keys_create_and_persist(priv_path, pub_path) != 0) {
			manifest_keys_set_error(err, err_len, "failed to create Ed25519 keypair");
			return -1;
		}
	}
	g_manifest_keys_loaded = 1;
	return 0;
}

/* Derive the keys directory (everything up to the last '/') of a key path.
 * Mirrors manifest_keys_ensure_dir's strrchr logic; -1 if no usable parent. */
static int manifest_keys_dir_of(const char *key_path, char *out, size_t out_sz)
{
	const char *slash;
	size_t len;

	slash = strrchr(key_path, '/');
	if (!slash || slash == key_path)
		return -1;
	len = (size_t)(slash - key_path);
	if (len >= out_sz)
		return -1;
	memcpy(out, key_path, len);
	out[len] = '\0';
	return 0;
}

int manifest_keys_rotate(char *err, size_t err_len)
{
	char priv_path[512];
	char pub_path[512];
	char keys_dir[512];
	uint8_t old_priv[CRYPTO_ED25519_PRIVATE_KEY_SIZE];
	uint8_t old_pub[CRYPTO_ED25519_PUBLIC_KEY_SIZE];
	int had_old_keys;
	time_t now;
	long long ts;

	manifest_keys_clear_loaded();
	if (manifest_keys_build_paths(priv_path, sizeof(priv_path),
			pub_path, sizeof(pub_path)) != 0) {
		manifest_keys_set_error(err, err_len, "failed to resolve keys directory");
		return -1;
	}
	if (access(priv_path, F_OK) == 0 && manifest_priv_permissions_ok(priv_path) != 0) {
		manifest_keys_set_error(err, err_len,
			"ed25519.priv permissions too open (expected 0600)");
		return -1;
	}
	had_old_keys = 0;
	if (access(priv_path, F_OK) == 0 && access(pub_path, F_OK) == 0) {
		if (manifest_read_key_file(priv_path, old_priv, sizeof(old_priv)) == 0 &&
		    manifest_read_key_file(pub_path, old_pub, sizeof(old_pub)) == 0)
			had_old_keys = 1;
	}
	now = time(NULL);
	if (now == (time_t)-1) {
		manifest_keys_set_error(err, err_len, "failed to read current time");
		return -1;
	}
	ts = (long long)now;
	if (manifest_keys_ensure_dir(priv_path) != 0) {
		manifest_keys_set_error(err, err_len, "failed to create keys directory");
		return -1;
	}
	if (crypto_ed25519_keypair(g_manifest_pubkey, g_manifest_privkey) != 0) {
		manifest_keys_set_error(err, err_len, "failed to generate Ed25519 keypair");
		return -1;
	}
	if (had_old_keys) {
		char bak_priv[576];
		char bak_pub[576];
		if (snprintf(bak_priv, sizeof(bak_priv), "%s.bak.%lld", priv_path, ts) >=
		    (int)sizeof(bak_priv) ||
		    snprintf(bak_pub, sizeof(bak_pub), "%s.bak.%lld", pub_path, ts) >=
		    (int)sizeof(bak_pub)) {
			manifest_keys_set_error(err, err_len, "backup path too long");
			return -1;
		}
		if (manifest_write_key_file(bak_priv, old_priv, sizeof(old_priv)) != 0) {
			manifest_keys_set_error(err, err_len, "failed to backup ed25519.priv");
			return -1;
		}
		if (manifest_write_key_file(bak_pub, old_pub, sizeof(old_pub)) != 0) {
			manifest_keys_set_error(err, err_len, "failed to backup ed25519.pub");
			return -1;
		}
	}
	if (manifest_write_key_file_atomic(priv_path, g_manifest_privkey,
			CRYPTO_ED25519_PRIVATE_KEY_SIZE) != 0) {
		manifest_keys_set_error(err, err_len, "failed to write ed25519.priv");
		return -1;
	}
	if (manifest_write_key_file_atomic(pub_path, g_manifest_pubkey,
			CRYPTO_ED25519_PUBLIC_KEY_SIZE) != 0) {
		manifest_keys_set_error(err, err_len, "failed to write ed25519.pub");
		if (had_old_keys) {
			/* Restore old_priv to disk (the new pub write failed atomically via
			 * rename, so the live pub is still old_pub) and reset in-memory
			 * state to the OLD keys so memory matches the on-disk pair instead
			 * of holding the rolled keys that never landed on disk. */
			if (manifest_write_key_file_atomic(priv_path, old_priv,
					sizeof(old_priv)) != 0)
				manifest_keys_set_error(err, err_len,
					"failed to restore ed25519.priv after pub write error");
			memcpy(g_manifest_pubkey, old_pub, sizeof(g_manifest_pubkey));
			memcpy(g_manifest_privkey, old_priv, sizeof(g_manifest_privkey));
			g_manifest_keys_loaded = 1;
		} else {
			unlink(priv_path);
			manifest_keys_clear_loaded();
		}
		return -1;
	}
	g_manifest_keys_loaded = 1;
	if (had_old_keys && manifest_keys_dir_of(priv_path, keys_dir, sizeof(keys_dir)) == 0)
		manifest_keys_prune_backups(keys_dir);
	return 0;
}

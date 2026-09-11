/**
 * @file skill.c
 * @brief Skill loader: scan skills directory for .md files and concatenate contents.
 * Hot-reload via inotify (Linux) or kqueue (macOS).
 */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "skill.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

#define SKILL_SEP "\n\n---\n\n"
#define PROMPT_SEP "\n\n"
#define READ_CHUNK 4096
#define MAX_PATH_LEN 1024
#define SKILL_CACHE_SIZE (32 * 1024)
#define WATCH_POLL_INTERVAL_MS 2000

static char *g_skills_cache;
static size_t g_skills_cache_len;
static pthread_mutex_t g_skills_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_watch_running;
static pthread_t g_watch_thread;
static const config_t *g_watch_cfg;
static int g_watch_verbose;
#if defined(__linux__)
static int g_watch_stop_pipe[2] = {-1, -1};
#endif

static int load_skills_from_disk(const config_t *cfg, char *out_buf, size_t out_size);

static void do_reload_cache(void)
{
	if (!g_watch_cfg) return;
	char *buf = malloc(SKILL_CACHE_SIZE);
	if (!buf) return;
	buf[0] = '\0';
	load_skills_from_disk(g_watch_cfg, buf, SKILL_CACHE_SIZE);
	pthread_mutex_lock(&g_skills_mutex);
	free(g_skills_cache);
	g_skills_cache = buf;
	g_skills_cache_len = strlen(buf);
	pthread_mutex_unlock(&g_skills_mutex);
}

#if defined(__linux__)
static void *watch_thread_fn(void *arg)
{
	(void)arg;
	const char *dir = config_skills_dir(g_watch_cfg);
	if (!dir || dir[0] == '\0') return NULL;
	int fd = inotify_init();
	if (fd < 0) return NULL;
	int wd = inotify_add_watch(fd, dir, IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	if (wd < 0) {
		close(fd);
		return NULL;
	}
	int max_fd = (fd > g_watch_stop_pipe[0]) ? fd : g_watch_stop_pipe[0];
	fd_set rfds;
	char buf[sizeof(struct inotify_event) + 256];
	while (g_watch_running) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		FD_SET(g_watch_stop_pipe[0], &rfds);
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(g_watch_stop_pipe[0], &rfds)) break;
		if (r > 0 && FD_ISSET(fd, &rfds)) {
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n > 0 && g_watch_running) {
				if (g_watch_verbose)
					fprintf(stderr, "skills: file change detected, reloading\n");
				do_reload_cache();
			}
		}
	}
	inotify_rm_watch(fd, wd);
	close(fd);
	return NULL;
}
#elif defined(__APPLE__)
static void *watch_thread_fn(void *arg)
{
	(void)arg;
	const char *dir = config_skills_dir(g_watch_cfg);
	if (!dir || dir[0] == '\0') return NULL;
	int dir_fd = open(dir, O_RDONLY);
	if (dir_fd < 0) return NULL;
	int kq = kqueue();
	if (kq < 0) {
		close(dir_fd);
		return NULL;
	}
	struct kevent kev;
	EV_SET(&kev, dir_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND, 0, NULL);
	if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
		close(kq);
		close(dir_fd);
		return NULL;
	}
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
	while (g_watch_running) {
		struct kevent ev;
		int n = kevent(kq, NULL, 0, &ev, 1, &ts);
		if (n > 0 && g_watch_running) {
			if (g_watch_verbose)
				fprintf(stderr, "skills: file change detected, reloading\n");
			do_reload_cache();
		}
	}
	close(kq);
	close(dir_fd);
	return NULL;
}
#else
static void *watch_thread_fn(void *arg)
{
	(void)arg;
	const char *dir = config_skills_dir(g_watch_cfg);
	if (!dir || dir[0] == '\0') return NULL;
	struct stat prev = {0};
	if (stat(dir, &prev) != 0) return NULL;
	while (g_watch_running) {
		usleep(1000 * (unsigned)WATCH_POLL_INTERVAL_MS);
		if (!g_watch_running) break;
		struct stat st;
		if (stat(dir, &st) != 0) continue;
		if (st.st_mtime != prev.st_mtime) {
			prev = st;
			if (g_watch_verbose)
				fprintf(stderr, "skills: directory change detected, reloading\n");
			do_reload_cache();
		}
	}
	return NULL;
}
#endif

static int ends_with_md(const char *name)
{
	size_t len = strlen(name);
	return len >= 3 && strcmp(name + len - 3, ".md") == 0;
}

static size_t append_str(char *out_buf, size_t out_size, size_t *used, const char *str)
{
	size_t len = strlen(str);
	size_t remain = out_size > *used ? out_size - *used - 1 : 0;
	if (remain == 0) return 0;
	if (len > remain) len = remain;
	memcpy(out_buf + *used, str, len);
	*used += len;
	out_buf[*used] = '\0';
	return len;
}

static size_t append_file_content(FILE *f, char *out_buf, size_t out_size, size_t *used)
{
	char buf[READ_CHUNK];
	size_t total = 0;
	while (*used < out_size - 1 && fgets(buf, (int)sizeof(buf), f) != NULL) {
		size_t n = strlen(buf);
		size_t remain = out_size - *used - 1;
		if (n > remain) n = remain;
		memcpy(out_buf + *used, buf, n);
		*used += n;
		out_buf[*used] = '\0';
		total += n;
	}
	return total;
}

static void append_file_by_path(const char *path, char *out_buf, size_t out_size, size_t *used)
{
	if (!path || path[0] == '\0') return;
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "warning: cannot read file for system prompt: %s\n", path);
		return;
	}
	append_file_content(f, out_buf, out_size, used);
	fclose(f);
}

static int load_skills_from_disk(const config_t *cfg, char *out_buf, size_t out_size)
{
	if (!cfg || !out_buf || out_size == 0) return -1;
	out_buf[0] = '\0';
	const char *dir_path = config_skills_dir(cfg);
	if (!dir_path || dir_path[0] == '\0') return 0;
	DIR *dir = opendir(dir_path);
	if (!dir) {
		if (errno == ENOENT || errno == ENOTDIR)
			fprintf(stderr, "warning: skills directory not found or not a directory: %s\n", dir_path);
		else
			fprintf(stderr, "warning: cannot open skills directory %s: %s\n", dir_path, strerror(errno));
		return 0;
	}
	size_t used = 0;
	int first = 1;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		if (!ends_with_md(ent->d_name)) continue;
		char path[MAX_PATH_LEN];
		int n = snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) continue;
		FILE *f = fopen(path, "r");
		if (!f) continue;
		if (!first) append_str(out_buf, out_size, &used, SKILL_SEP);
		first = 0;
		append_file_content(f, out_buf, out_size, &used);
		fclose(f);
	}
	closedir(dir);
	return 0;
}

int skill_load_all(const config_t *cfg, char *out_buf, size_t out_size)
{
	if (!cfg || !out_buf || out_size == 0) return -1;
	if (g_watch_running && g_skills_cache) {
		pthread_mutex_lock(&g_skills_mutex);
		if (g_skills_cache) {
			size_t n = g_skills_cache_len;
			if (n >= out_size) n = out_size - 1;
			memcpy(out_buf, g_skills_cache, n);
			out_buf[n] = '\0';
			pthread_mutex_unlock(&g_skills_mutex);
			return 0;
		}
		pthread_mutex_unlock(&g_skills_mutex);
	}
	return load_skills_from_disk(cfg, out_buf, out_size);
}

int skill_build_system_prompt_base(const config_t *cfg, const char *skills_content,
                                  char *out_buf, size_t out_size)
{
	if (!cfg || !out_buf || out_size == 0) return -1;
	out_buf[0] = '\0';
	size_t used = 0;
	const char *soul = config_agent_soul_path(cfg);
	if (soul && soul[0] != '\0') {
		append_file_by_path(soul, out_buf, out_size, &used);
		append_str(out_buf, out_size, &used, PROMPT_SEP);
	}
	const char *identity = config_agent_identity_path(cfg);
	if (identity && identity[0] != '\0') {
		append_file_by_path(identity, out_buf, out_size, &used);
		append_str(out_buf, out_size, &used, PROMPT_SEP);
	}
	if (skills_content && skills_content[0] != '\0')
		append_str(out_buf, out_size, &used, skills_content);
	return 0;
}

static int is_safe_skill_name(const char *name)
{
	if (!name || name[0] == '\0') return 0;
	if (name[0] == '.') return 0;
	for (const char *p = name; *p; p++) {
		if (*p == '/' || *p == '\\') return 0;
		if (p[0] == '.' && p[1] == '.') return 0;
	}
	return 1;
}

static int build_skill_path(const config_t *cfg, const char *name, char *path_out, size_t path_size)
{
	if (!cfg || !name || !path_out || path_size == 0) return -1;
	if (!is_safe_skill_name(name)) return -1;
	const char *dir = config_skills_dir(cfg);
	if (!dir || dir[0] == '\0') return -1;
	int n = snprintf(path_out, path_size, "%s/%s.md", dir, name);
	return (n < 0 || (size_t)n >= path_size) ? -1 : 0;
}

int skill_list_names(const config_t *cfg, char **names_out, int max_count)
{
	if (!cfg || !names_out || max_count <= 0) return -1;
	const char *dir_path = config_skills_dir(cfg);
	if (!dir_path || dir_path[0] == '\0') return 0;
	DIR *dir = opendir(dir_path);
	if (!dir) return 0;
	int count = 0;
	struct dirent *ent;
	while (count < max_count && (ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		if (!ends_with_md(ent->d_name)) continue;
		size_t len = strlen(ent->d_name);
		if (len <= 3) continue;
		names_out[count] = strndup(ent->d_name, len - 3);
		if (!names_out[count]) {
			for (int i = 0; i < count; i++) free(names_out[i]);
			closedir(dir);
			return -1;
		}
		count++;
	}
	closedir(dir);
	return count;
}

int skill_get_content(const config_t *cfg, const char *name, char *out_buf, size_t out_size)
{
	if (!cfg || !name || !out_buf || out_size == 0) return -1;
	char path[MAX_PATH_LEN];
	if (build_skill_path(cfg, name, path, sizeof(path)) != 0) return -1;
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	out_buf[0] = '\0';
	size_t used = 0;
	append_file_content(f, out_buf, out_size, &used);
	fclose(f);
	return 0;
}

int skill_get_description(const config_t *cfg, const char *name, char *out_buf, size_t out_size)
{
	const char *override;
	char line[512];
	char *nl;
	const char *start;
	size_t len;

	if (!cfg || !name || !out_buf || out_size == 0) return -1;
	override = config_asap_skill_description(cfg, name);
	if (override && override[0] != '\0') {
		snprintf(out_buf, out_size, "%s", override);
		return 0;
	}
	if (build_skill_path(cfg, name, line, sizeof(line)) != 0) return -1;
	{
		FILE *f = fopen(line, "r");
		if (!f) return -1;
		if (!fgets(line, (int)sizeof(line), f)) {
			fclose(f);
			return -1;
		}
		fclose(f);
	}
	nl = strchr(line, '\n');
	if (nl) *nl = '\0';
	start = line;
	while (*start == ' ' || *start == '\t') start++;
	if (start[0] == '#' && start[1] == ' ') start += 2;
	else if (start[0] == '#') start += 1;
	while (*start == ' ' || *start == '\t') start++;
	if (start[0] == '\0') return -1;
	len = strlen(start);
	if (len >= out_size) len = out_size - 1;
	memcpy(out_buf, start, len);
	out_buf[len] = '\0';
	return 0;
}

int skill_create(const config_t *cfg, const char *name, const char *content)
{
	if (!cfg || !name || !content) return -1;
	char path[MAX_PATH_LEN];
	if (build_skill_path(cfg, name, path, sizeof(path)) != 0) return -1;
	FILE *f = fopen(path, "r");
	if (f) {
		fclose(f);
		return -1;
	}
	f = fopen(path, "w");
	if (!f) return -1;
	size_t len = strlen(content);
	size_t written = fwrite(content, 1, len, f);
	fclose(f);
	return (written == len) ? 0 : -1;
}

int skill_update(const config_t *cfg, const char *name, const char *content)
{
	if (!cfg || !name || !content) return -1;
	char path[MAX_PATH_LEN];
	if (build_skill_path(cfg, name, path, sizeof(path)) != 0) return -1;
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	size_t len = strlen(content);
	size_t written = fwrite(content, 1, len, f);
	fclose(f);
	return (written == len) ? 0 : -1;
}

int skill_delete(const config_t *cfg, const char *name)
{
	if (!cfg || !name) return -1;
	char path[MAX_PATH_LEN];
	if (build_skill_path(cfg, name, path, sizeof(path)) != 0) return -1;
	return remove(path) == 0 ? 0 : -1;
}

int skill_watch_start(const config_t *cfg, int verbose)
{
	if (!cfg) return -1;
	const char *dir = config_skills_dir(cfg);
	if (!dir || dir[0] == '\0') return -1;
	if (g_watch_running) return 0;
#if defined(__linux__)
	if (pipe(g_watch_stop_pipe) != 0) return -1;
#endif
	g_watch_cfg = cfg;
	g_watch_verbose = verbose;
	do_reload_cache();
	if (!g_skills_cache) {
		char buf[SKILL_CACHE_SIZE];
		buf[0] = '\0';
		load_skills_from_disk(cfg, buf, sizeof(buf));
		pthread_mutex_lock(&g_skills_mutex);
		g_skills_cache = strdup(buf);
		g_skills_cache_len = g_skills_cache ? strlen(g_skills_cache) : 0;
		pthread_mutex_unlock(&g_skills_mutex);
	}
	g_watch_running = 1;
	if (pthread_create(&g_watch_thread, NULL, watch_thread_fn, NULL) != 0) {
		g_watch_running = 0;
		pthread_mutex_lock(&g_skills_mutex);
		free(g_skills_cache);
		g_skills_cache = NULL;
		pthread_mutex_unlock(&g_skills_mutex);
		return -1;
	}
	return 0;
}

void skill_watch_stop(void)
{
	if (!g_watch_running) return;
	g_watch_running = 0;
#if defined(__linux__)
	if (g_watch_stop_pipe[1] >= 0) {
		if (write(g_watch_stop_pipe[1], "x", 1) < 0) { /* best-effort on shutdown */ }
		close(g_watch_stop_pipe[1]);
		g_watch_stop_pipe[1] = -1;
	}
#endif
	pthread_join(g_watch_thread, NULL);
#if defined(__linux__)
	if (g_watch_stop_pipe[0] >= 0) {
		close(g_watch_stop_pipe[0]);
		g_watch_stop_pipe[0] = -1;
	}
#endif
	pthread_mutex_lock(&g_skills_mutex);
	free(g_skills_cache);
	g_skills_cache = NULL;
	g_skills_cache_len = 0;
	pthread_mutex_unlock(&g_skills_mutex);
	g_watch_cfg = NULL;
}

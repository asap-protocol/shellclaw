/**
 * @file main.c
 * @brief Entry point: CLI, config, init order, signals, main loop.
 *
 * **Daemon / systemd (PRD §4.4):** `--daemon` uses a classic double-fork and session detach so the
 * process stays in the background and returns the parent shell immediately. User services should
 * prefer `Type=simple` with **no** `--daemon` so systemd tracks the main PID directly; `--daemon` is
 * for manual runs (PID file + log under `~/.shellclaw/`). The PID file uses a POSIX advisory write
 * lock via `fcntl(F_SETLK)` (portable equivalent to `flock()` for single-instance semantics).
 *
 * **SIGHUP / PRD §9 Q4:** Reload re-parses `config.toml` (with env overrides) and swaps the in-memory
 * `config_t *`. We refresh **live pointers** for the router, tools, gateway HTTP context, heartbeat,
 * Telegram, and Discord so ongoing work reads new settings. We **do not** re-`init` providers or
 * channels (no llama key re-probe, no Telegram/Discord reconnect, no memory DB reopen, no gateway
 * rebind). Changes that need that require a process restart.
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest_keys.h"
#include "core/bootstrap.h"
#include "core/config.h"
#include "core/daemon.h"
#include "core/agent.h"
#include "core/dispatch.h"
#include "core/reload.h"
#include "core/version.h"
#include "channels/channel.h"
#include "hardware/board_detect.h"
#include "providers/provider.h"
#include "tools/cron.h"
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG_PATH "~/.shellclaw/config.toml"
#define POLL_TIMEOUT_MS 1000

static volatile sig_atomic_t g_shutdown;
static const char *g_cli_one_shot;
static int g_verbose;
static int g_want_daemon;
static int g_headless;

static void on_signal(int sig)
{
	(void)sig;
	g_shutdown = 1;
}

static void setup_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) != 0)
		fprintf(stderr, "warning: sigaction(SIGINT) failed\n");
	if (sigaction(SIGTERM, &sa, NULL) != 0)
		fprintf(stderr, "warning: sigaction(SIGTERM) failed\n");
	sa.sa_handler = on_hup;
	if (sigaction(SIGHUP, &sa, NULL) != 0)
		fprintf(stderr, "warning: sigaction(SIGHUP) failed\n");
}

static int has_background_channels(void)
{
	for (int i = 0; i < bootstrap_channel_count(); i++) {
		const channel_t *channel = bootstrap_channel_at(i);
		if (channel != channel_cli_get() && channel != channel_cron_get())
			return 1;
	}
	return 0;
}

/* Older systemd units use /dev/null for stdin and start ShellClaw without --headless. */
static int uses_systemd_null_input(void)
{
	const char *invocation_id = getenv("INVOCATION_ID");
	struct stat input, null_device;
	return invocation_id && invocation_id[0] && fstat(STDIN_FILENO, &input) == 0 &&
	       S_ISCHR(input.st_mode) && stat("/dev/null", &null_device) == 0 &&
	       input.st_rdev == null_device.st_rdev;
}

static int main_loop(int one_shot, config_t **pcfg)
{
	int cli_closed = g_headless || g_want_daemon;
	int keep_running = cli_closed || has_background_channels() || (!one_shot && uses_systemd_null_input());
	while (!g_shutdown) {
		if (g_reload_requested) {
			g_reload_requested = 0;
			agent_lock();
			try_config_reload(pcfg);
			agent_unlock();
		}
		provider_router_periodic_recovery_tick(time(NULL));
		channel_incoming_msg_t msg;
		memset(&msg, 0, sizeof(msg));
		int got = 0;
		const channel_t *which = NULL;
		int nch = bootstrap_channel_count();
		for (int i = 0; i < nch && !got; i++) {
			const channel_t *ch = bootstrap_channel_at(i);
			if (!ch || (cli_closed && ch == channel_cli_get()))
				continue;
			int r = ch->poll(&msg, POLL_TIMEOUT_MS);
			if (r == CHANNEL_POLL_CLOSED) {
				cli_closed = 1;
				if (!keep_running)
					return one_shot ? EXIT_FAILURE : EXIT_SUCCESS;
				continue;
			}
			if (r == 1) {
				got = 1;
				which = ch;
				break;
			}
			if (r < 0)
				channel_incoming_msg_clear(&msg);
		}
		if (got && which) {
			int result = handle_message(which, &msg);
			channel_incoming_msg_clear(&msg);
			if (one_shot)
				return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		} else if (cli_closed && !g_shutdown) {
			/* Cron and heartbeat can return at once when no job is due. */
			const struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
			(void)nanosleep(&delay, NULL);
		}
	}
	return one_shot ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void print_usage(FILE *stream, const char *prog)
{
	fprintf(stream,
	        "Usage: %s [--config <path>] [--verbose] [--daemon | --headless] [--detect-board] [--rotate-keys] [--version] [--help] [-m \"message\"]\n",
	        prog);
}

static int parse_args(int argc, char **argv, const char **config_path_out)
{
	*config_path_out = DEFAULT_CONFIG_PATH;
	g_cli_one_shot = NULL;
	g_want_daemon = 0;
	daemon_set_want(0);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage(stdout, argv[0]);
			puts("Run with --headless to keep the process open without terminal input.");
			exit(0);
		}
		if (strcmp(argv[i], "--version") == 0) {
			printf("%s\n", SHELLCLAW_RELEASE_VERSION);
			exit(0);
		}
		if (strcmp(argv[i], "--detect-board") == 0) {
			printf("%s\n", board_name(board_detect()));
			exit(0);
		}
		if (strcmp(argv[i], "--rotate-keys") == 0) {
			char errbuf[256] = {0};
			if (manifest_keys_rotate(errbuf, sizeof(errbuf)) != 0) {
				fprintf(stderr, "Error: %s\n",
				        errbuf[0] ? errbuf : "key rotation failed");
				exit(1);
			}
			puts("rotation complete; refresh your marketplace listing");
			exit(0);
		}
		if (strcmp(argv[i], "--verbose") == 0) {
			g_verbose = 1;
			continue;
		}
		if (strcmp(argv[i], "--daemon") == 0) {
			g_want_daemon = 1;
			daemon_set_want(1);
			continue;
		}
		if (strcmp(argv[i], "--headless") == 0) {
			g_headless = 1;
			continue;
		}
		if (strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: --config requires a path argument\n");
				return -1;
			}
			*config_path_out = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-m") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: -m requires a message argument\n");
				return -1;
			}
			g_cli_one_shot = argv[++i];
			continue;
		}
		fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
		print_usage(stderr, argv[0]);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *config_path;
	if (parse_args(argc, argv, &config_path) != 0) return 1;
	config_t *cfg = NULL;
	char errbuf[256] = {0};
	if (config_load(config_path, &cfg, errbuf, sizeof(errbuf)) != 0) {
		fprintf(stderr, "Error: %s\n", errbuf[0] ? errbuf : "failed to load config");
		return 1;
	}
	if (g_headless && g_cli_one_shot) {
		fprintf(stderr, "Error: Use -m without --headless.\n");
		config_free(cfg);
		return 1;
	}
	if (g_want_daemon && g_cli_one_shot) {
		fprintf(stderr, "Error: --daemon cannot be used together with -m\n");
		config_free(cfg);
		return 1;
	}
	if (enter_daemon_mode() != 0) {
		config_free(cfg);
		return 1;
	}
	g_shutdown = 0;
	bootstrap_set_config_path(config_path);
	bootstrap_set_verbose(g_verbose);
	bootstrap_set_cli_one_shot(g_cli_one_shot);
	setup_signals();
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		fprintf(stderr, "Error: curl_global_init failed\n");
		config_free(cfg);
		return 1;
	}
	if (init_subsystems(cfg) != 0) {
		fprintf(stderr, "Error: subsystem init failed (check config, API keys, memory path)\n");
		curl_global_cleanup();
		config_free(cfg);
		return 1;
	}
	int result = main_loop(g_cli_one_shot != NULL, &cfg);
	{
		config_t *live = bootstrap_get_cfg();
		cleanup_subsystems();
		curl_global_cleanup();
		daemon_pid_cleanup();
		stale_free_all();
		config_free(live);
	}
	return result;
}

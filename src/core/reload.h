/**
 * @file reload.h
 * @brief SIGHUP config reload and stale config queue.
 */

#ifndef SHELLCLAW_RELOAD_H
#define SHELLCLAW_RELOAD_H

#include "core/config.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Set by on_hup(); cleared by main loop before try_config_reload(). */
extern volatile sig_atomic_t g_reload_requested;

/** SIGHUP handler: request config reload on next main-loop iteration. */
void on_hup(int sig);

/** Queue old config for deferred free after reload. @return 0 on success, -1 on OOM. */
int stale_enqueue(config_t *old);

/** Free all queued stale configs. */
void stale_free_all(void);

/**
 * Re-parse config and swap live pointers. Old config is queued via stale_enqueue().
 * Callers on different threads must hold agent_lock() around this call.
 * @param pcfg In/out active config pointer (updated on success).
 * @return 0 on success, -1 if reload did not swap the live config.
 */
int try_config_reload(config_t **pcfg);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_RELOAD_H */

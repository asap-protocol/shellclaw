/**
 * @file sandbox_env.c
 * @brief Secret-free environment for the sandboxed command process.
 *
 * Provider keys and channel tokens live in the agent environment. The
 * shell tool's stdout is returned to the model, so sh -c must not inherit
 * them. A prompt-injected printenv would otherwise leak the values.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sandbox/sandbox_env.h"

#include <stdlib.h>
#include <string.h>
#if !defined(__linux__)
#include <unistd.h>
#endif

static const char FALLBACK_PATH[] =
    "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

static char *dup_env(const char *name)
{
    const char *v;

    v = getenv(name);
    if (!v || !v[0])
        return NULL;
    return strdup(v);
}

static void put_env(const char *name, const char *value)
{
    if (value && value[0])
        setenv(name, value, 1);
}

static void free_kept(char *path, char *lang, char *lc_all, char *lc_ctype,
                      char *term, char *tz)
{
    free(path);
    free(lang);
    free(lc_all);
    free(lc_ctype);
    free(term);
    free(tz);
}

void sandbox_scrub_command_environment(const char *workspace)
{
    char *path = dup_env("PATH");
    char *lang = dup_env("LANG");
    char *lc_all = dup_env("LC_ALL");
    char *lc_ctype = dup_env("LC_CTYPE");
    char *term = dup_env("TERM");
    char *tz = dup_env("TZ");
#if !defined(__linux__)
    static char *empty_env[] = { NULL };
    extern char **environ;
#endif

#if defined(__linux__)
    clearenv();
#else
    environ = empty_env;
#endif
    put_env("PATH", path ? path : FALLBACK_PATH);
    put_env("LANG", lang);
    put_env("LC_ALL", lc_all);
    put_env("LC_CTYPE", lc_ctype);
    put_env("TERM", term);
    put_env("TZ", tz);
    if (workspace && workspace[0])
        setenv("HOME", workspace, 1);
    free_kept(path, lang, lc_all, lc_ctype, term, tz);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2004 - 2012 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <locale.h>

#include <glib/gstdio.h>

#include "main-utils.h"
#include "NetworkManagerUtils.h"
#include "nm-config.h"
#include "libnm-glib-aux/nm-io-utils.h"

static gboolean
sighup_handler(gpointer user_data)
{
    nm_main_config_reload(GPOINTER_TO_INT(user_data));
    return G_SOURCE_CONTINUE;
}

static gboolean
sigint_handler(gpointer user_data)
{
    GMainLoop *main_loop = user_data;

    nm_log_info(LOGD_CORE, "caught SIGINT, shutting down normally.");
    g_main_loop_quit(main_loop);

    return G_SOURCE_REMOVE;
}

static gboolean
sigterm_handler(gpointer user_data)
{
    GMainLoop *main_loop = user_data;

    nm_log_info(LOGD_CORE, "caught SIGTERM, shutting down normally.");
    g_main_loop_quit(main_loop);

    return G_SOURCE_REMOVE;
}

/**
 * nm_main_utils_setup_signals:
 * @main_loop: the #GMainLoop to quit when SIGINT or SIGTERM is received
 *
 * Sets up signal handling for NetworkManager.
 */
void
nm_main_utils_setup_signals(GMainLoop *main_loop)
{
    g_return_if_fail(main_loop != NULL);

    signal(SIGPIPE, SIG_IGN);

    g_unix_signal_add(SIGHUP, sighup_handler, GINT_TO_POINTER(SIGHUP));
    if (nm_glib_check_version(2, 36, 0)) {
        g_unix_signal_add(SIGUSR1, sighup_handler, GINT_TO_POINTER(SIGUSR1));
        g_unix_signal_add(SIGUSR2, sighup_handler, GINT_TO_POINTER(SIGUSR2));
    } else
        nm_log_warn(LOGD_CORE,
                    "glib-version: cannot handle SIGUSR1 and SIGUSR2 signals. Consider upgrading "
                    "glib to 2.36.0 or newer");
    g_unix_signal_add(SIGINT, sigint_handler, main_loop);
    g_unix_signal_add(SIGTERM, sigterm_handler, main_loop);
}

gboolean
nm_main_utils_write_pidfile(const char *pidfile)
{
    gs_free_error GError *error = NULL;
    char                  pid[16];

    nm_sprintf_buf(pid, "%lld", (long long) getpid());
    if (!nm_utils_file_set_contents(pidfile, pid, -1, 00644, NULL, NULL, &error)) {
        fprintf(stderr, _("Writing to %s failed: %s\n"), pidfile, error->message);
        return FALSE;
    }

    return TRUE;
}

void
nm_main_utils_ensure_statedir(void)
{
    gs_free char *parent = NULL;
    int           errsv;

    parent = g_path_get_dirname(NMSTATEDIR);

    /* Ensure parent state directories exists */
    if (parent && parent[0] == '/' && parent[1] != '\0'
        && g_mkdir_with_parents(parent, 0755) != 0) {
        errsv = errno;
        fprintf(stderr,
                "Cannot create parents for '%s': %s",
                NMSTATEDIR,
                nm_strerror_native(errsv));
        exit(1);
    }
    /* Ensure state directory exists */
    if (g_mkdir_with_parents(NMSTATEDIR, 0700) != 0) {
        errsv = errno;
        fprintf(stderr, "Cannot create '%s': %s", NMSTATEDIR, nm_strerror_native(errsv));
        exit(1);
    }
}

void
nm_main_utils_ensure_rundir(void)
{
    int errsv;

    /* Setup runtime directory */
    if (g_mkdir_with_parents(NMRUNDIR, 0755) != 0) {
        errsv = errno;
        fprintf(stderr, _("Cannot create '%s': %s"), NMRUNDIR, nm_strerror_native(errsv));
        exit(1);
    }

    /* NM_CONFIG_DEVICE_STATE_DIR is used to determine whether NM is restarted or not.
     * It is important to set NMConfigCmdLineOptions.first_start before creating
     * the directory. */
    nm_assert(g_str_has_prefix(NM_CONFIG_DEVICE_STATE_DIR, NMRUNDIR "/"));
    if (g_mkdir(NM_CONFIG_DEVICE_STATE_DIR, 0755) != 0) {
        errsv = errno;
        if (errsv != EEXIST) {
            fprintf(stderr,
                    _("Cannot create '%s': %s"),
                    NM_CONFIG_DEVICE_STATE_DIR,
                    nm_strerror_native(errsv));
            exit(1);
        }
    }
}

/**
 * nm_main_utils_ensure_not_running_pidfile:
 * @pidfile: the pid file
 *
 * Checks whether the pidfile already exists and contains PID of a running
 * process.
 *
 * Exits with code 1 if a conflicting process is running.
 */
void
nm_main_utils_ensure_not_running_pidfile(const char *pidfile)
{
    gs_free char *contents     = NULL;
    gs_free char *proc_cmdline = NULL;
    gsize         len          = 0;
    pid_t         pid;
    gint64        pid64;
    const char   *process_name;
    const char   *prgname = g_get_prgname();

    g_return_if_fail(prgname);

    if (!pidfile || !*pidfile)
        return;

    if (!g_file_get_contents(pidfile, &contents, &len, NULL))
        return;
    if (len <= 0)
        return;

    errno = 0;
    pid64 = _nm_utils_ascii_str_to_int64(contents, 10, 0, G_MAXINT64, -1);
    pid   = (pid_t) pid64;
    if (pid <= 0 || (gint64) pid != pid64)
        return;

    nm_clear_g_free(&contents);
    proc_cmdline = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/cmdline", (gint64) pid);
    if (!g_file_get_contents(proc_cmdline, &contents, &len, NULL))
        return;

    process_name = strrchr(contents, '/');
    if (process_name)
        process_name++;
    else
        process_name = contents;
    if (strcmp(process_name, prgname) == 0) {
        /* Check that the process exists */
        if (kill(pid, 0) == 0) {
            fprintf(stderr, _("%s is already running (pid %lld)\n"), prgname, (long long) pid);
            exit(1);
        }
    }
}

gboolean
nm_main_utils_early_setup(const char   *progname,
                          int          *argc,
                          char        **argv[],
                          GOptionEntry *options,
                          void (*option_context_hook)(gpointer user_data, GOptionContext *opt_ctx),
                          gpointer    option_context_hook_data,
                          const char *summary)
{
    GOptionContext *opt_ctx = NULL;
    GError         *error   = NULL;
    gboolean        success = FALSE;
    int             i;
    const char     *opt_fmt_log_level = NULL, *opt_fmt_log_domains = NULL;
    const char    **opt_loc_log_level = NULL, **opt_loc_log_domains = NULL;

    /* Make GIO ignore the remote VFS service; otherwise it tries to use the
     * session bus to contact the remote service, and NM shouldn't ever be
     * talking on the session bus.  See rh #588745
     */
    setenv("GIO_USE_VFS", "local", 1);

    /*
     * Set the umask to 0022, which results in 0666 & ~0022 = 0644.
     * Otherwise, if root (or an su'ing user) has a wacky umask, we could
     * write out an unreadable resolv.conf.
     */
    umask(022);

    /* Ensure gettext() gets the right environment (bgo #666516) */
    setlocale(LC_ALL, "");
    textdomain(GETTEXT_PACKAGE);

    for (i = 0; options[i].long_name; i++) {
        NM_PRAGMA_WARNING_DISABLE("-Wformat-nonliteral")
        if (!strcmp(options[i].long_name, "log-level")) {
            opt_fmt_log_level = options[i].description;
            opt_loc_log_level = &options[i].description;
            options[i].description =
                g_strdup_printf(options[i].description, nm_logging_all_levels_to_string());
        } else if (!strcmp(options[i].long_name, "log-domains")) {
            opt_fmt_log_domains = options[i].description;
            opt_loc_log_domains = &options[i].description;
            options[i].description =
                g_strdup_printf(options[i].description, nm_logging_all_domains_to_string());
        }
        NM_PRAGMA_WARNING_REENABLE
    }

    /* Parse options */
    opt_ctx = g_option_context_new(NULL);
    g_option_context_set_translation_domain(opt_ctx, GETTEXT_PACKAGE);
    g_option_context_set_ignore_unknown_options(opt_ctx, FALSE);
    g_option_context_set_help_enabled(opt_ctx, TRUE);
    g_option_context_add_main_entries(opt_ctx, options, NULL);
    g_option_context_set_summary(opt_ctx, summary);
    if (option_context_hook)
        option_context_hook(option_context_hook_data, opt_ctx);

    success = g_option_context_parse(opt_ctx, argc, argv, &error);
    if (!success) {
        fprintf(stderr,
                _("%s.  Please use --help to see a list of valid options.\n"),
                error->message);
        g_clear_error(&error);
    }
    g_option_context_free(opt_ctx);

    if (opt_loc_log_level) {
        g_free((char *) *opt_loc_log_level);
        *opt_loc_log_level = opt_fmt_log_level;
    }
    if (opt_loc_log_domains) {
        g_free((char *) *opt_loc_log_domains);
        *opt_loc_log_domains = opt_fmt_log_domains;
    }

    return success;
}

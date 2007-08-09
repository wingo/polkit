/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/***************************************************************************
 *
 * polkit-grant-helper.c : setgid polkituser grant helper for PolicyKit
 *
 * Copyright (C) 2007 David Zeuthen, <david@fubar.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 *
 **************************************************************************/

/* TODO: FIXME: XXX: this code needs security review before it can be released! */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <security/pam_appl.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <utime.h>

#include <glib.h>

#include <polkit-dbus/polkit-dbus.h>
#include <polkit/polkit-grant-database.h>

/* Development aid: define PGH_DEBUG to get debugging output. Do _NOT_
 * enable this in production builds; it may leak passwords and other
 * sensitive information.
 */
#undef PGH_DEBUG
/* #define PGH_DEBUG */

/* synopsis: polkit-grant-helper <pid> <action-name>
 *
 * <pid>           : process id of caller to grant privilege to
 * <action-name>   : the PolicyKit action
 *
 * Error/debug messages goes to stderr. Interaction with the program
 * launching this helper happens via stdin/stdout. A rough high-level
 * interaction diagram looks like this (120 character width):
 *
 *  Program using
 *  libpolkit-grant                    polkit-grant-helper                  polkit-grant-helper-pam
 *  -------------                      -------------------                  -----------------------
 *
 *   Spawn polkit-grant-helper
 *   with args <pid>, <action-name> -->
 *
 *                                   Create PolKitCaller object
 *                                   from <pid>. Involves querying
 *                                   ConsoleKit over the system
 *                                   message-bus. Verify that
 *                                   the caller qualifies for
 *                                   for authentication to gain
 *                                   the right to do the Action.
 *
 *                      <-- Tell libpolkit-grant about grant details, e.g.
 *                          {self,admin}_{,keep_session,keep_always}
 *                          using stdout
 *
 *   Receive grant details on stdin.
 *   Caller prepares UI dialog depending
 *   on grant details.
 *
 *                                       Spawn polkit-grant-helper-pam
 *                                       with no args -->
 *
 *                                       Write username to auth as
 *                                       on stdout -->
 *                                        
 *                                                                         Receive username on stdin.
 *                                                                         Start the PAM stack
 * auth_in_progess:
 *                                                                         Write a PAM request on stdout, one off
 *                                                                         - PAM_PROMPT_ECHO_OFF
 *                                                                         - PAM_PROMPT_ECHO_ON
 *                                                                         - PAM_ERROR_MSG
 *                                                                         - PAM_TEXT_INFO
 *
 *                                       Receive PAM request on stdin.
 *                                       Send it to libpolkit-grant on stdout
 *
 *   Receive PAM request on stdin.
 *   Program deals with it.
 *   Write reply on stdout
 *
 *                                       Receive PAM reply on stdin
 *                                       Send PAM reply on stdout
 *
 *                                                                         Deal with PAM reply on stdin.
 *                                                                         Now either
 *                                                                         - GOTO auth_in_progress; or
 *                                                                         - Write SUCCESS|FAILURE on stdout and then
 *                                                                           die
 *                                                                         
 *                                       Receive either SUCCESS or
 *                                       FAILURE on stdin. If FAILURE
 *                                       is received, then die with exit
 *                                       code 1. If SUCCESS, leave a cookie
 *                                       in /var/{lib,run}/PolicyKit indicating
 *                                       the grant was successful and die with
 *                                       exit code 0
 *
 *
 * If auth fails, we exit with code 1.
 * If input is not valid we exit with code 2.
 * If any other error occur we exit with code 3
 * If privilege was granted, we exit code 0.
 */


/* the authentication itself is done via a setuid root helper; this is
 * to make the code running as uid 0 easier to audit. */
static polkit_bool_t
do_auth (const char *user_to_auth)
{
        int helper_pid;
        int helper_stdin;
        int helper_stdout;
        GError *g_error;
        char *helper_argv[2] = {PACKAGE_LIBEXEC_DIR "/polkit-grant-helper-pam", NULL};
        char buf[256];
        FILE *child_stdin;
        FILE *child_stdout;
        gboolean ret;

        child_stdin = NULL;
        child_stdout = NULL;
        ret = FALSE;

        g_error = NULL;
        if (!g_spawn_async_with_pipes (NULL,
                                       (char **) helper_argv,
                                       NULL,
                                       0,
                                       NULL,
                                       NULL,
                                       &helper_pid,
                                       &helper_stdin,
                                       &helper_stdout,
                                       NULL,
                                       &g_error)) {
                fprintf (stderr, "polkit-grant-helper: cannot spawn helper: %s\n", g_error->message);
                g_error_free (g_error);
                g_free (helper_argv[1]);
                goto out;
        }

        child_stdin = fdopen (helper_stdin, "w");
        if (child_stdin == NULL) {
                fprintf (stderr, "polkit-grant-helper: fdopen (helper_stdin) failed: %s\n", strerror (errno));
                goto out;
        }
        child_stdout = fdopen (helper_stdout, "r");
        if (child_stdout == NULL) {
                fprintf (stderr, "polkit-grant-helper: fdopen (helper_stdout) failed: %s\n", strerror (errno));
                goto out;
        }

        /* First, tell the pam helper what user we wish to auth */
        fprintf (child_stdin, "%s\n", user_to_auth);
        fflush (child_stdin);

        /* now act as middle man between our parent and our child */

        while (TRUE) {
                /* read from child */
                if (fgets (buf, sizeof buf, child_stdout) == NULL)
                        goto out;
#ifdef PGH_DEBUG
                fprintf (stderr, "received: '%s' from child; sending to parent\n", buf);
#endif /* PGH_DEBUG */
                /* see if we're done? */
                if (strcmp (buf, "SUCCESS\n") == 0) {
                        ret = TRUE;
                        goto out;
                }
                if (strcmp (buf, "FAILURE\n") == 0) {
                        goto out;
                }
                /* send to parent */
                fprintf (stdout, buf);
                fflush (stdout);
                
                /* read from parent */
                if (fgets (buf, sizeof buf, stdin) == NULL)
                        goto out;
#ifdef PGH_DEBUG
                fprintf (stderr, "received: '%s' from parent; sending to child\n", buf);
#endif /* PGH_DEBUG */
                /* send to child */
                fprintf (child_stdin, buf);
                fflush (child_stdin);
        }

out:
        if (child_stdin != NULL)
                fclose (child_stdin);
        if (child_stdout != NULL)
                fclose (child_stdout);
        return ret;
}

static polkit_bool_t
verify_with_polkit (pid_t caller_pid,
                    const char *action_name,
                    PolKitResult *result,
                    char **out_session_objpath)
{
        PolKitCaller *caller;
        PolKitSession *session;
        char *str;
        DBusConnection *bus;
        DBusError error;
        PolKitContext *pol_ctx;
        PolKitAction *action;

        dbus_error_init (&error);
        bus = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
        if (bus == NULL) {
                fprintf (stderr, "polkit-grant-helper: cannot connect to system bus: %s: %s\n", 
                         error.name, error.message);
                dbus_error_free (&error);
                goto error;
        }

        action = polkit_action_new ();
        polkit_action_set_action_id (action, action_name);

        caller = polkit_caller_new_from_pid (bus, caller_pid, &error);
        if (caller == NULL) {
                fprintf (stderr, "polkit-grant-helper: cannot get caller from pid\n");
                goto error;
        }

        if (!polkit_caller_get_ck_session (caller, &session)) {
                fprintf (stderr, "polkit-grant-helper: caller is not in a session\n");
                goto error;
        }
        if (!polkit_session_get_ck_objref (session, &str)) {
                fprintf (stderr, "polkit-grant-helper: cannot get session ck objpath\n");
                goto error;
        }
        *out_session_objpath = g_strdup (str);
        if (*out_session_objpath == NULL)
                goto error;

        pol_ctx = polkit_context_new ();
        if (!polkit_context_init (pol_ctx, NULL)) {
                fprintf (stderr, "polkit-grant-helper: cannot initialize polkit\n");
                goto error;
        }

        *result = polkit_context_can_caller_do_action (pol_ctx, action, caller);

        if (*result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH &&
            *result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION &&
            *result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS &&
            *result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH &&
            *result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION &&
            *result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS) {
                fprintf (stderr, "polkit-grant-helper: given auth type (%d -> %s) is bogus\n", 
                         *result, polkit_result_to_string_representation (*result));
                goto error;
        }

        /* TODO: we should probably clean up */

        return TRUE;
error:
        return FALSE;
}

static polkit_bool_t
get_and_validate_override_details (PolKitResult *result)
{
        char buf[256];
        PolKitResult desired_result;

        if (fgets (buf, sizeof buf, stdin) == NULL)
                goto error;
        if (strlen (buf) > 0 &&
            buf[strlen (buf) - 1] == '\n')
                buf[strlen (buf) - 1] = '\0';
        
        fprintf (stderr, "polkit-grant-helper: caller said '%s'\n", buf);

        if (!polkit_result_from_string_representation (buf, &desired_result))
                goto error;

        fprintf (stderr, "polkit-grant-helper: testing for voluntarily downgrade from '%s' to '%s'\n",
                 polkit_result_to_string_representation (*result),
                 polkit_result_to_string_representation (desired_result));

        /* See the huge comment in main() below... 
         *
         * it comes down to this... users can only choose a more restricted granting type...
         */
        switch (*result) {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH)
                        goto error;
                break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION)
                        goto error;
                break;
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS)
                        goto error;
                break;

        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH)
                        goto error;
                break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION)
                        goto error;
                break;
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
                if (desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION &&
                    desired_result != POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS)
                        goto error;
                break;

        default:
                /* we should never reach this */
                goto error;
        }

        if (*result != desired_result) {
                fprintf (stderr, "polkit-grant-helper: voluntarily downgrading from '%s' to '%s'\n",
                         polkit_result_to_string_representation (*result),
                         polkit_result_to_string_representation (desired_result));
        }

        *result = desired_result;

        return TRUE;
error:
        return FALSE;
}

int
main (int argc, char *argv[])
{
        int ret;
        uid_t invoking_user_id;
        pid_t caller_pid;
        gid_t egid;
        struct group *group;
        char *endp;
        const char *invoking_user_name;
        const char *action_name;
        PolKitResult result;
        const char *user_to_auth;
        char *session_objpath;
        struct passwd *pw;
        polkit_bool_t dbres;

        ret = 3;

        /* clear the entire environment to avoid attacks using with libraries honoring environment variables */
        if (clearenv () != 0)
                goto out;
        /* set a minimal environment */
        setenv ("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);

        openlog ("polkit-grant-helper", LOG_CONS | LOG_PID, LOG_AUTHPRIV);

        /* check for correct invocation */
        if (argc != 3) {
                syslog (LOG_NOTICE, "inappropriate use of helper, wrong number of arguments [uid=%d]", getuid ());
                fprintf (stderr, "polkit-grant-helper: wrong number of arguments. This incident has been logged.\n");
                goto out;
        }

        /* check we're running with a non-tty stdin */
        if (isatty (STDIN_FILENO) != 0) {
                syslog (LOG_NOTICE, "inappropriate use of helper, stdin is a tty [uid=%d]", getuid ());
                fprintf (stderr, "polkit-grant-helper: inappropriate use of helper, stdin is a tty. This incident has been logged.\n");
                goto out;
        }

        /* check user */
        invoking_user_id = getuid ();
        if (invoking_user_id == 0) {
                fprintf (stderr, "polkit-grant-helper: it only makes sense to run polkit-grant-helper as non-root\n");
                goto out;
        }

        /* check that we are setgid polkituser */
        egid = getegid ();
        group = getgrgid (egid);
        if (group == NULL) {
                fprintf (stderr, "polkit-grant-helper: cannot lookup group info for gid %d\n", egid);
                goto out;
        }
        if (strcmp (group->gr_name, POLKIT_GROUP) != 0) {
                fprintf (stderr, "polkit-grant-helper: needs to be setgid " POLKIT_GROUP "\n");
                goto out;
        }

        pw = getpwuid (invoking_user_id);
        if (pw == NULL) {
                fprintf (stderr, "polkit-grant-helper: cannot lookup passwd info for uid %d\n", invoking_user_id);
                goto out;
        }
        invoking_user_name = strdup (pw->pw_name);
        if (invoking_user_name == NULL) {
                fprintf (stderr, "polkit-grant-helper: OOM allocating memory for invoking user name\n");
                goto out;
        }

        caller_pid = strtol (argv[1], &endp, 10);
        if (endp == NULL || endp == argv[1] || *endp != '\0') {
                fprintf (stderr, "polkit-grant-helper: cannot parse pid\n");
                goto out;
        }
        action_name = argv[2];

#ifdef PGH_DEBUG
        fprintf (stderr, "polkit-grant-helper: invoking user   = %d ('%s')\n", invoking_user_id, invoking_user_name);
        fprintf (stderr, "polkit-grant-helper: caller_pid      = %d\n", caller_pid);
        fprintf (stderr, "polkit-grant-helper: action_name     = '%s'\n", action_name);
#endif /* PGH_DEBUG */

        ret = 2;

        /* Use libpolkit to
         *
         * - figure out if the caller can really auth to do the action
         * - learn what ConsoleKit session the caller belongs to
         */
        if (!verify_with_polkit (caller_pid, action_name, &result, &session_objpath))
                goto out;

#ifdef PGH_DEBUG
        fprintf (stderr, "polkit-grant-helper: polkit result   = '%s'\n", 
                 polkit_result_to_string_representation (result));
        fprintf (stderr, "polkit-grant-helper: session_objpath = '%s'\n", session_objpath);
#endif /* PGH_DEBUG */

        /* tell the caller about the grant details; e.g. whether
         * it's auth_self_keep_always or auth_self etc.
         */
        fprintf (stdout, "POLKIT_GRANT_HELPER_TELL_TYPE %s\n", 
                 polkit_result_to_string_representation (result));
        fflush (stdout);

        /* figure out what user to auth */
        if (result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH ||
            result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION ||
            result == POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS) {
                /* TODO: with wheel support, figure out what user to auth */
                user_to_auth = "root";
        } else {
                user_to_auth = invoking_user_name;
        }

        ret = 1;

        /* Start authentication */
        if (!do_auth (user_to_auth))
                goto out;

        /* Ask caller if he want to slim down grant type...  e.g. he
         * might want to go from auth_self_keep_always to
         * auth_self_keep_session..
         *
         * See docs for the PolKitGrantOverrideGrantType callback type
         * for use cases; it's in polkit-grant/polkit-grant.h
         */
        fprintf (stdout, "POLKIT_GRANT_HELPER_ASK_OVERRIDE_GRANT_TYPE %s\n", 
                 polkit_result_to_string_representation (result));
        fflush (stdout);
        
        if (!get_and_validate_override_details (&result)) {
                /* if this fails it means bogus input from user */
                ret = 2;
                goto out;
        }

#ifdef PGH_DEBUG
        fprintf (stderr, "polkit-grant-helper: adding grant: action_id=%s session_id=%s pid=%d result='%s'\n", 
                 action_name, session_objpath, caller_pid, polkit_result_to_string_representation (result));
#endif /* PGH_DEBUG */

        switch (result) {
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH:
                dbres = _polkit_grantdb_write_pid (action_name, caller_pid);
                if (dbres) {
                        syslog (LOG_INFO, "granted use of action='%s' to pid '%d' [uid=%d] [auth='%s']",
                                action_name, caller_pid, invoking_user_id, user_to_auth);
                }
                break;

        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_SESSION:
        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_SESSION:
                dbres = _polkit_grantdb_write_keep_session (action_name, session_objpath);
                if (dbres) {
                        syslog (LOG_INFO, "granted use of action='%s' to session '%s' [uid=%d] [auth='%s']",
                                action_name, session_objpath, invoking_user_id, user_to_auth);
                }
                break;

        case POLKIT_RESULT_ONLY_VIA_SELF_AUTH_KEEP_ALWAYS:
        case POLKIT_RESULT_ONLY_VIA_ADMIN_AUTH_KEEP_ALWAYS:
                dbres = _polkit_grantdb_write_keep_always (action_name, invoking_user_id);
                if (dbres) {
                        syslog (LOG_INFO, "granted use of action='%s' to uid %d [auth='%s']", 
                                action_name, invoking_user_id, user_to_auth);
                }
                break;

        default:
                /* should never happen */
                goto out;
        }

        if (!dbres) {
                fprintf (stderr, "polkit-grant-helper: failed to write to grantdb\n");
                goto out;
        }

        /* touch the /var/lib/PolicyKit/reload file */
        if (utime (PACKAGE_LOCALSTATE_DIR "/lib/PolicyKit/reload", NULL) != 0) {
                fprintf (stderr, "polkit-grant-helper: cannot touch " PACKAGE_LOCALSTATE_DIR "/lib/PolicyKit/reload: %s\n", strerror (errno));
        }


        ret = 0;
out:
#ifdef PGH_DEBUG
        fprintf (stderr, "polkit-grant-helper: exiting with code %d\n", ret);
#endif /* PGH_DEBUG */
        return ret;
}
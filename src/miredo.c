/*
 * miredo.c - Miredo common daemon functions
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright © 2004-2007 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license, or (at  *
 *  your option) any later version.                                    *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gettext.h>

#include <string.h> // memset(), strsignal()
#include <stdlib.h> // exit()
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>

#include <pthread.h> // pthread_sigmask()
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h> // uid_t
#include <sys/wait.h> // waitpid()
#ifdef HAVE_SYS_CAPABILITY_H
# include <sys/capability.h>
#endif

#ifndef LOG_PERROR
# define LOG_PERROR 0
#endif

#include "miredo.h"
#include "conf.h"

uid_t unpriv_uid = 0;
const char *miredo_chrootdir = NULL;

extern int
drop_privileges (void)
{
	/*
	 * We could chroot earlier, but we do it know to keep compatibility with
	 * grsecurity Linux kernel patch that automatically removes capabilities
	 * when chrooted.
	 */
	if ((miredo_chrootdir != NULL)
	 && (chroot (miredo_chrootdir) || chdir ("/")))
	{
		syslog (LOG_ALERT, _("Error (%s): %m"), "chroot");
		return -1;
	}

	// Definitely drops privileges
	if (setuid (unpriv_uid))
	{
		syslog (LOG_ALERT, _("Error (%s): %m"), "setuid");
		return -1;
	}

#ifdef HAVE_LIBCAP
	cap_t s = cap_init ();
	if (s != NULL)
	{
		cap_set_proc (s);
		cap_free (s);
	}
#endif
	return 0;
}


/*
 * Configuration and respawning stuff
 */
static void logger (void *dummy, bool error, const char *fmt, va_list ap)
{
	(void)dummy;

	vsyslog (error ? LOG_ERR : LOG_WARNING, fmt, ap);
}


extern int
miredo (const char *confpath, const char *server_name, int pidfd)
{
	sigset_t set, exit_set, reload_set;
	int retval;
	miredo_conf *cnf = miredo_conf_create (logger, NULL);

	if (cnf == NULL)
		return -1;

	sigemptyset (&set);

	/* Exit signals */
	sigaddset (&set, SIGINT);
	sigaddset (&set, SIGQUIT);
	sigaddset (&set, SIGTERM);
	exit_set = set;

	/* Reload signal */
	sigaddset (&set, SIGHUP);
	reload_set = set;

	/* No-op signal */
	sigaddset (&set, SIGCHLD);

	pthread_sigmask (SIG_BLOCK, &set, NULL);

	openlog (miredo_name, LOG_PID | LOG_PERROR, LOG_DAEMON);

	do
	{
		int facility = LOG_DAEMON;
		retval = 1;

		if (!miredo_conf_read_file (cnf, confpath))
			syslog (LOG_WARNING, _("Loading configuration from %s failed"),
			        confpath);

		miredo_conf_parse_syslog_facility (cnf, "SyslogFacility",
		                                   &facility);

		closelog ();
		openlog (miredo_name, LOG_PID | LOG_PERROR, facility);
		syslog (LOG_INFO, _("Starting..."));

		// Starts the main miredo process
		pid_t pid = fork ();

		switch (pid)
		{
			case -1:
				syslog (LOG_ALERT, _("Error (%s): %m"), "fork");
				continue;

			case 0:
				close (pidfd);
				retval = miredo_run (cnf, server_name);
				miredo_conf_destroy (cnf);
				closelog ();
				exit (-retval);
				break;

			default:
				miredo_conf_clear (cnf, 0);
		}

		int status, signum;

		for (;;)
		{
			while (sigwait (&set, &signum));

			if (signum == SIGCHLD
			 && waitpid (pid, &status, WNOHANG) == pid)
				break; /* child died */

			if (sigismember (&exit_set, signum))
			{
				syslog (LOG_NOTICE, _("Exiting on signal %d (%s)"),
				        signum, strsignal (signum));
				retval = 0;
			}
			else
			if (sigismember (&reload_set, signum))
			{
				syslog (LOG_NOTICE,
				        _("Reloading configuration on signal %d (%s)"),
				        signum, strsignal (signum));
				retval = 2;
			}
			else
				continue;

			// Tells children to exit */
			kill (pid, SIGTERM);
			// Waits until the miredo process terminates
			while (waitpid (pid, &status, 0) != pid);
			break;
		}

		// At this point, the child process is gone.
		if (WIFEXITED (status))
		{
			status = WEXITSTATUS (status);
			syslog (LOG_NOTICE, _("Child %d exited (code: %d)"),
			        (int)pid, status);
			if (status)
				retval = 1;
		}
		else
		if (WIFSIGNALED (status))
		{
			status = WTERMSIG (status);
			syslog (LOG_INFO, _("Child %d killed by signal %d (%s)"),
			        (int)pid, status, strsignal (status));
			retval = 2;
			sleep (1);
		}
	}
	while (retval == 2);

	miredo_conf_destroy (cnf);

	syslog (LOG_INFO, gettext (retval
		? N_("Terminated with error(s).")
		: N_("Terminated with no error.")));

	closelog ();
	return -retval;
}


int (*miredo_diagnose) (void);
int (*miredo_run) (miredo_conf *conf, const char *server);

const char *miredo_name;

# ifdef HAVE_LIBCAP
const cap_value_t *miredo_capv;
int miredo_capc;
# endif


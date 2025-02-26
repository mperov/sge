/*	$Id: rshd.c,v 1.23 2009-08-20 20:20:44 ravinallan Exp $	*/

/*-
 * Copyright (c) 1988, 1989, 1992, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * remote shell server:
 *	[port]\0
 *	remuser\0
 *	locuser\0
 *	command\0
 *	data
 */

/*#if defined ALPHA5
#define _POSIX_PII_SOCKET
#endif
*/

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>

#include <grp.h>

#ifdef SOLARIS
#include <sys/filio.h>
#endif

#include <sge_unistd.h>
#include <setosjobid.h>
#include <setrlimits.h>
#include <config_file.h>
#include <sge_uidgid.h>

#if defined SOLARIS || HPUX || NECSX5 || CRAY
#define _PATH_NOLOGIN "/etc/nologin"
#define _PATH_BSHELL "/bin/sh"
#define _PATH_DEFPATH "/usr/bin:/bin"
#else
#include <paths.h>
#endif

#if defined AIX || ALPHA || IRIX
#define _PATH_DEFPATH "/usr/bin:/bin"
#endif

#if defined __CYGWIN__
#define _PATH_NOLOGIN "/etc/nologin"
#endif

#if defined ALPHA4 || HP10 || IRIX || (SOLARIS && !HAS_SOCKLEN_T) || NECSX5 || CRAY || DARWIN6
typedef int socklen_t;
#endif

#if defined HP10 || HP1164 || LINUX || NECSX5 || CRAY 
#ifndef HAS_IN_PORT_T
typedef unsigned short in_port_t;
#endif
#endif

#if defined(IRIX) || defined(INTERIX) || defined(__CYGWIN__)
#  define NCARGS ARG_MAX
#endif

int	keepalive = 1;
int	check_all;
int   check_nologin = 1;
int	log_success;		/* If TRUE, log all successful accesses */
int	sent_null;

bool  g_new_interactive_job_support = false; /* This is needed in err_trace.c */

extern int foreground;

#ifndef __P
#define __P(protos) protos
#endif

static void	 doit __P((struct sockaddr_in *));
static void	 error __P((const char *, ...));
static void	 getstr __P((char *, int, char *));
static int	 local_domain __P((char *));
static char	*topdomain __P((char *));
static void	 usage __P((void));
int	main __P((int, char *[]));

#define	OPTIONS	"ahilnL"

int check_rhosts_file = 1;
   
int
main(argc, argv)
	int argc;
	char *argv[];
{
	struct linger linger;
	int ch, on = 1;
   socklen_t fromlen;
	struct sockaddr_in from;

	openlog("rshd", LOG_PID | LOG_ODELAY, LOG_DAEMON);

	opterr = 0;
	while ((ch = getopt(argc, argv, OPTIONS)) != -1)
		switch (ch) {
		case 'a':
			check_all = 1;
			break;
		case 'l':
			check_rhosts_file = 0;
			break;
		case 'n':
			keepalive = 0;
			break;
		case 'L':
			log_success = 1;
			break;
		case 'i':
			check_nologin = 0;
			break;
		case 'h':
		default:
			usage();
			break;
		}

	argc -= optind;
	argv += optind;


	fromlen = sizeof (from);
	if (getpeername(0, (struct sockaddr *)&from, &fromlen) < 0) {
		syslog(LOG_ERR, "getpeername: %m");
		_exit(1);
	}
	if (keepalive &&
	    setsockopt(0, SOL_SOCKET, SO_KEEPALIVE, (char *)&on,
	    sizeof(on)) < 0)
		syslog(LOG_WARNING, "setsockopt (SO_KEEPALIVE): %m");
	linger.l_onoff = 1;
	linger.l_linger = 60;			/* XXX */
	if (setsockopt(0, SOL_SOCKET, SO_LINGER, (char *)&linger,
	    sizeof (linger)) < 0)
		syslog(LOG_WARNING, "setsockopt (SO_LINGER): %m");
	doit(&from);
	/* NOTREACHED */
#ifdef __GNUC__
	exit(0);
#endif
   return 0;
}

char	username[20] = "USER=";
char	homedir[64] = "HOME=";
char	shell[64] = "SHELL=";
char	path[100] = "PATH=";
char	*envinit[] =
	    {homedir, shell, path, username, 0};
char	**environ;

static void
doit(fromp)
	struct sockaddr_in *fromp;
{
/*	extern char *__rcmd_errstr;*/	/* syslog hook from libc/net/rcmd.c. */
	struct hostent *hp;
	struct passwd *pwd;
	in_port_t port;
	fd_set ready, readfrom;
	int cc, nfd, pv[2], pid, s = -1;	/* XXX gcc */
	int one = 1;
	char *hostname, *errorstr, *errorhost = NULL;	/* XXX gcc */
	const char *cp;
	char sig, buf[BUFSIZ];
	char cmdbuf[NCARGS+1], locuser[16], remuser[16];
	char remotehost[2 * MAXHOSTNAMELEN + 1];
	char hostnamebuf[2 * MAXHOSTNAMELEN + 1];
        /* char active_jobs_dir[SGE_PATH_MAX]; */
        char *s_qsub_gid = NULL;
        char err_str[2048];


	(void) signal(SIGINT, SIG_DFL);
	(void) signal(SIGQUIT, SIG_DFL);
	(void) signal(SIGTERM, SIG_DFL);
#ifdef DEBUG
	{ int t = open(_PATH_TTY, 2);
	  if (t >= 0) {
		ioctl(t, TIOCNOTTY, (char *)0);
		(void) close(t);
	  }
	}
#endif
	fromp->sin_port = ntohs((in_port_t)fromp->sin_port);
	if (fromp->sin_family != AF_INET) {
		syslog(LOG_ERR, "malformed \"from\" address (af %d)\n",
		    fromp->sin_family);
		exit(1);
	}
#ifdef IP_OPTIONS
      {
	u_char optbuf[BUFSIZ/3], *cp;
	char lbuf[BUFSIZ], *lp;
	socklen_t optsize = sizeof(optbuf); 
   int ipproto;
	struct protoent *ip;

	if ((ip = getprotobyname("ip")) != NULL)
		ipproto = ip->p_proto;
	else
		ipproto = IPPROTO_IP;
	if (!getsockopt(0, ipproto, IP_OPTIONS, (char *)optbuf, &optsize) &&
	    optsize != 0) {
		lp = lbuf;
		for (cp = optbuf; optsize > 0; cp++, optsize--, lp += 3)
			sprintf(lp, " %2.2x", *cp);
		syslog(LOG_NOTICE,
		    "Connection received from %s using IP options (ignored):%s",
		    inet_ntoa(fromp->sin_addr), lbuf);
		if (setsockopt(0, ipproto, IP_OPTIONS,
		    (char *)NULL, optsize) != 0) {
			syslog(LOG_ERR, "setsockopt IP_OPTIONS NULL: %m");
			exit(1);
		}
	}
      }
#endif

	if (fromp->sin_port >= IPPORT_RESERVED ||
	    fromp->sin_port < IPPORT_RESERVED/2) {
		syslog(LOG_NOTICE|LOG_AUTH,
		    "Connection from %s on illegal port %u",
		    inet_ntoa(fromp->sin_addr), fromp->sin_port);
		exit(1);
	}

	(void) alarm(60);
	port = 0;
	for (;;) {
		char c;

		if ((cc = read(STDIN_FILENO, &c, 1)) != 1) {
			if (cc < 0)
				syslog(LOG_NOTICE, "read: %m");
			shutdown(0, 1+1);
			exit(1);
		}
		if (c == 0)
			break;
		port = port * 10 + c - '0';
	}

	(void) alarm(0);
	if (port != 0) {
		int lport = IPPORT_RESERVED - 1;
      int tries = 0;
      while ((s = rresvport(&lport)) < 0 && errno == EAGAIN && tries++ < 20)
         sleep(tries);
		if (s < 0) {
			syslog(LOG_ERR, "can't get stderr port: %m");
			exit(1);
		}
		if (port >= IPPORT_RESERVED) {
			syslog(LOG_ERR, "2nd port not reserved\n");
			exit(1);
		}
		fromp->sin_port = htons(port);
		if (connect(s, (struct sockaddr *)fromp, sizeof (*fromp)) < 0) {
			syslog(LOG_INFO, "connect second port %d: %m", port);
			exit(1);
		}
	}


#ifdef notdef
	/* from inetd, socket is already on 0, 1, 2 */
	dup2(f, 0);
	dup2(f, 1);
	dup2(f, 2);
#endif
	errorstr = NULL;
	hp = gethostbyaddr((char *)&fromp->sin_addr, sizeof (struct in_addr),
	    fromp->sin_family);
	if (hp) {
		/*
		 * If name returned by gethostbyaddr is in our domain,
		 * attempt to verify that we haven't been fooled by someone
		 * in a remote net; look up the name and check that this
		 * address corresponds to the name.
		 */
		hostname = (char *) hp->h_name; /* cast for Cygwin's const name */
		if (check_all || local_domain((char *) hp->h_name)) {
			strncpy(remotehost, hp->h_name, sizeof(remotehost) - 1);
			remotehost[sizeof(remotehost) - 1] = 0;
			errorhost = remotehost;
			hp = gethostbyname(remotehost);
			if (hp == NULL) {
				syslog(LOG_INFO,
				    "Couldn't look up address for %s",
				    remotehost);
				errorstr =
				"Couldn't look up address for your host (%s)\n";
				hostname = inet_ntoa(fromp->sin_addr);
			} else for (; ; hp->h_addr_list++) {
				if (hp->h_addr_list[0] == NULL) {
					syslog(LOG_NOTICE,
					  "Host addr %s not listed for host %s",
					    inet_ntoa(fromp->sin_addr),
					    hp->h_name);
					errorstr =
					    "Host address mismatch for %s\n";
					hostname = inet_ntoa(fromp->sin_addr);
					break;
				}
				if (!memcmp(hp->h_addr_list[0],
				    (caddr_t)&fromp->sin_addr,
				    sizeof(fromp->sin_addr))) {
					hostname = hp->h_name;
					break;
				}
			}
		}
		hostname = strncpy(hostnamebuf, hostname,
		    sizeof(hostnamebuf) - 1);
	} else
		errorhost = hostname = strncpy(hostnamebuf,
		    inet_ntoa(fromp->sin_addr), sizeof(hostnamebuf) - 1);

	hostnamebuf[sizeof(hostnamebuf) - 1] = '\0';

	getstr(remuser, sizeof(remuser), "remuser");
	getstr(locuser, sizeof(locuser), "locuser");
	getstr(cmdbuf, sizeof(cmdbuf), "command");
	setpwent();
   
   /* we are now in active job directory - read config before we change
   ** to user directory
   ** and initialize admin user
   */
   {
      read_config("config");
      if(sge_set_admin_username(get_conf_val("admin_user"), err_str,
                                sizeof(err_str))) {
         errorstr = err_str;
         goto fail;
      }   
      s_qsub_gid = get_conf_val("qsub_gid");
   }
   
	pwd = getpwnam(locuser);
	if (pwd == NULL) {
		syslog(LOG_INFO|LOG_AUTH,
		    "%s@%s as %s: unknown login. cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
		if (errorstr == NULL)
			errorstr = "Login incorrect.\n";
		goto fail;
	}

   /* if(getcwd(active_jobs_dir, SGE_PATH_MAX) == NULL) {
      error("cannot determine active_jobs directory\n");
      exit(1);
   } */
   
	/* if (chdir(pwd->pw_dir) < 0) {
		syslog(LOG_INFO|LOG_AUTH,
		    "%s@%s as %s: no home directory. cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
		error("No remote directory.\n");
		exit(1);
	} */


	if (errorstr ||
	    (pwd->pw_passwd != 0 && *pwd->pw_passwd != '\0' &&
		check_rhosts_file && ruserok(hostname, pwd->pw_uid == 0, remuser, locuser) < 0)) { 
          
		if (errno != 0)
			syslog(LOG_INFO|LOG_AUTH,
			    "%s@%s as %s: permission denied (%s). cmd='%.80s'",
			    remuser, hostname, locuser, strerror(errno),
			    cmdbuf);
		else
			syslog(LOG_INFO|LOG_AUTH,
			    "%s@%s as %s: permission denied. cmd='%.80s'",
			    remuser, hostname, locuser, cmdbuf);
fail:
		if (errorstr == NULL)
			errorstr = "Permission denied.\n";
		error(errorstr, errorhost);
		exit(1);
	}

	if (check_nologin && pwd->pw_uid && !access(_PATH_NOLOGIN, F_OK)) {
		error("Logins currently disabled.\n");
		exit(1);
	}

	(void) write(STDERR_FILENO, "\0", 1);
	sent_null = 1;

	if (port) {
		if (pipe(pv) < 0) {
			error("Can't make pipe.\n");
			exit(1);
		}
		pid = fork();
		if (pid == -1)  {
			error("Can't fork; try again.\n");
			exit(1);
		}
		if (pid) {
			{
				(void) close(0);
				(void) close(1);
			}
			(void) close(2);
			(void) close(pv[1]);

			FD_ZERO(&readfrom);
			FD_SET(s, &readfrom);
			FD_SET(pv[0], &readfrom);
			if (pv[0] > s)
				nfd = pv[0];
			else
				nfd = s;
			ioctl(pv[0], FIONBIO, (char *)&one);

			/* should set s nbio! */
			nfd++;
			do {
				ready = readfrom;
				if (select(nfd, &ready, (fd_set *)0,
				    (fd_set *)0, (struct timeval *)0) < 0)
					break;
				if (FD_ISSET(s, &ready)) {
					int	ret;

					ret = read(s, &sig, 1);
					if (ret <= 0)
						FD_CLR(s, &readfrom);
					else
						killpg(pid, sig);
				}
				if (FD_ISSET(pv[0], &ready)) {
					errno = 0;
					cc = read(pv[0], buf, sizeof(buf));
					if (cc <= 0) {
						shutdown(s, 1+1);
						FD_CLR(pv[0], &readfrom);
					} else {
						(void) write(s, buf, cc);
					}
				}

			} while (FD_ISSET(s, &readfrom) ||
			    FD_ISSET(pv[0], &readfrom));
         /* parent exits, when select on filehandles (stderr pipe from child and 
         ** control port to rsh client) fails, or succeeds but nothing to read.
         ** We should wait for the child to exit!
         */
         waitpid(pid, NULL, 0); 
		   exit(0);
		}

      SETPGRP;
   
		(void) close(s);
		(void) close(pv[0]);
		dup2(pv[1], 2);
		close(pv[1]);
	}

{
   gid_t add_grp_id;
   gid_t old_grp_id;
   
   /* chdir(active_jobs_dir); */
   sge_switch2admin_user();
   foreground = 0; /* setosjobid shall write to shepherd trace file */
   setosjobid(0, &add_grp_id, pwd);
   setrlimits(0);
   sge_switch2start_user();
   /* chdir(pwd->pw_dir); */
   
	if (*pwd->pw_shell == '\0')
		pwd->pw_shell = _PATH_BSHELL;
#if	BSD > 43
	if (setlogin(pwd->pw_name) < 0)
		syslog(LOG_ERR, "setlogin() failed: %m");
#endif
   /*
    * preserve the old primary gid for initgroups()
    * see cr 6590010
    */
   old_grp_id = pwd->pw_gid;

   if(s_qsub_gid != NULL && strcmp(s_qsub_gid, "no") != 0) {
      pwd->pw_gid = atoi(s_qsub_gid);
   }

	(void) setgid((gid_t)pwd->pw_gid);
	initgroups(pwd->pw_name, old_grp_id);
   
#if (SOLARIS || ALPHA || LINUX || DARWIN)     
   /* add Additional group id to current list of groups */
   if (add_grp_id) {
      bool skip_silently = false;
      const char *tmp_str = search_conf_val("skip_ngroups_max_silently");

      if (tmp_str != NULL && strcmp(tmp_str, "yes") == 0) {
         skip_silently = true;
      }
      if (sge_add_group(add_grp_id, err_str, sizeof(err_str),
                        skip_silently) == -1) {
         error(err_str);
      }
   }
#endif
   
}
	(void) setuid((uid_t)pwd->pw_uid);
   
	if (chdir(pwd->pw_dir) < 0) {
		syslog(LOG_INFO|LOG_AUTH,
		    "%s@%s as %s: no home directory. cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
		error("No remote directory.\n");
		exit(1);
	} 

	environ = envinit;
	strncat(homedir, pwd->pw_dir, sizeof(homedir)-6);
	strcat(path, _PATH_DEFPATH);
	strncat(shell, pwd->pw_shell, sizeof(shell)-7);
	strncat(username, pwd->pw_name, sizeof(username)-6);
	cp = strrchr(pwd->pw_shell, '/');
	if (cp)
		cp++;
	else
		cp = pwd->pw_shell;
	endpwent();
	if (log_success || pwd->pw_uid == 0) {
		syslog(LOG_INFO|LOG_AUTH, "%s@%s as %s: cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
	}
	execl(pwd->pw_shell, cp, "-c", cmdbuf, (char *) NULL);
	perror(pwd->pw_shell);
	exit(1);
}

/*
 * Report error to client.  Note: can't be used until second socket has
 * connected to client, or older clients will hang waiting for that
 * connection first.
 */
#include <stdarg.h>

static void
error(const char *fmt, ...)
{
   va_list ap;
   int len;
   char *bp, buf[BUFSIZ];
   va_start(ap, fmt);
   bp = buf;
   if (sent_null == 0) {
      *bp++ = 1;
      len = 1;
   } else
      len = 0;
#if defined ALPHA4 || HP10 || IRIX || (SOLARIS && ! SOLARIS64) 
   vsprintf(bp, fmt, ap);
#else   
   vsnprintf(bp, sizeof(buf) - 1, fmt, ap);
#endif   
   write(STDERR_FILENO, buf, len + strlen(bp));
}


static void
getstr(buf, cnt, err)
	char *buf, *err;
	int cnt;
{
	char c;

	do {
		if (read(STDIN_FILENO, &c, 1) != 1)
			exit(1);
		*buf++ = c;
		if (--cnt == 0) {
			error("%s too long\n", err);
			exit(1);
		}
	} while (c != 0);
}

/*
 * Check whether host h is in our local domain,
 * defined as sharing the last two components of the domain part,
 * or the entire domain part if the local domain has only one component.
 * If either name is unqualified (contains no '.'),
 * assume that the host is local, as it will be
 * interpreted as such.
 */
static int
local_domain(h)
	char *h;
{
	char localhost[MAXHOSTNAMELEN + 1];
	char *p1, *p2;

	localhost[0] = 0;
	(void)gethostname(localhost, sizeof(localhost));
	localhost[sizeof(localhost) - 1] = '\0';
	p1 = topdomain(localhost);
	p2 = topdomain(h);
	if (p1 == NULL || p2 == NULL || !strcasecmp(p1, p2))
		return (1);
	return (0);
}

static char *
topdomain(h)
	char *h;
{
	char *p, *maybe = NULL;
	int dots = 0;

	for (p = h + strlen(h); p >= h; p--) {
		if (*p == '.') {
			if (++dots == 2)
				return (p);
			maybe = p;
		}
	}
	return (maybe);
}

static void
usage()
{

	syslog(LOG_ERR, "usage: rshd [-%s]", OPTIONS);
	exit(2);
}

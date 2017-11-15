/*
 *  Copyright (C) 2017 Savoir-Faire Linux Inc.
 *
 *  Authors:
 *       Gaël PORTAY <gael.portay@savoirfairelinux.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
# include "config.h"
#else
const char VERSION[] = __DATE__ " " __TIME__;
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <assert.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <asm/types.h>
#include <linux/netlink.h>

static char *rcS[] = { "/etc/init.d/rcS", "start", NULL };
static char *sh[] = { "-sh", NULL };

#define __strncmp(s1, s2) strncmp(s1, s2, sizeof(s2) - 1)
#define __close(fd) do { \
	int __error = errno; \
	if (close(fd) == -1) \
		perror("close"); \
	errno = __error; \
} while(0)

#ifndef UEVENT_BUFFER_SIZE
#define UEVENT_BUFFER_SIZE 2048
#endif

static int nl_fd;
int netlink_open(struct sockaddr_nl *addr, int signal);
ssize_t netlink_recv(int fd, struct sockaddr_nl *addr);
int netlink_close(int fd);

typedef int uevent_event_cb_t(char *, char *, void *);
typedef int uevent_variable_cb_t(char *, char *, void *);
int uevent_parse_line(char *line,
		      uevent_event_cb_t *evt_cb,
		      uevent_variable_cb_t *var_cb,
		      void *data);

struct options_t {
	int argc;
	char * const *argv;
};

static inline const char *applet(const char *arg0)
{
	char *s = strrchr(arg0, '/');
	if (!s)
		return arg0;

	return s+1;
}

void usage(FILE * f, char * const arg0)
{
	fprintf(f, "Usage: %s [OPTIONS]\n\n"
		   "Options:\n"
		   " -V or --version        Display the version.\n"
		   " -h or --help           Display this message.\n"
		   "", applet(arg0));
}

int run(const char *path, char * const argv[], const char *devname)
{
	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		return -1;
	}

	/* Parent */
	if (pid) {
		int status;

		if (waitpid(pid, &status, 0) == -1) {
			perror("waitpid");
			return -1;
		}

		if (WIFEXITED(status))
			status = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			fprintf(stderr, "%s\n", strsignal(WTERMSIG(status)));

		return status;
	}

	netlink_close(nl_fd);

	/* Child */
	if (devname) {
		int fd;

		if (chdir("/dev") == -1)
			perror("chdir");

		close(STDIN_FILENO);
		fd = open(devname, O_RDONLY|O_NOCTTY);
		if (fd == -1)
			perror("open");

		close(STDOUT_FILENO);
		fd = open(devname, O_WRONLY|O_NOCTTY);
		if (fd == -1)
			perror("open");

		close(STDERR_FILENO);
		dup2(STDOUT_FILENO, STDERR_FILENO);

		if (chdir("/") == -1)
			perror("chdir");
	}

	execv(path, argv);
	_exit(127);
}

int spawn(const char *path, char * const argv[], const char *devname)
{
	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		return -1;
	}

	/* Parent */
	if (pid) {
		int status;

		if (waitpid(pid, &status, 0) == -1) {
			perror("waitpid");
			return -1;
		}

		if (WIFEXITED(status))
			status = WEXITSTATUS(status);
		else if (WIFSIGNALED(status))
			fprintf(stderr, "%s\n", strsignal(WTERMSIG(status)));

		return status;
	}

	netlink_close(nl_fd);

	/* Child */
	pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid) {
		exit(EXIT_SUCCESS);
	}

	/* Daemon */
	if (devname) {
		int fd;

		if (chdir("/dev") == -1)
			perror("chdir");

		close(STDIN_FILENO);
		fd = open(devname, O_RDONLY|O_NOCTTY);
		if (fd == -1)
			perror("open");

		close(STDOUT_FILENO);
		fd = open(devname, O_WRONLY|O_NOCTTY);
		if (fd == -1)
			perror("open");

		close(STDERR_FILENO);
		dup2(STDOUT_FILENO, STDERR_FILENO);

		if (chdir("/") == -1)
			perror("chdir");
	}

	execv(path, argv);
	_exit(127);
}

int parse_arguments(struct options_t *opts, int argc, char * const argv[])
{
	static const struct option long_options[] = {
		{ "version", no_argument,       NULL, 'V' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL,      no_argument,       NULL, 0   }
	};

	opterr = 0;
	for (;;) {
		int index;
		int c = getopt_long(argc, argv, "Vh", long_options, &index);
		if (c == -1)
			break;

		switch (c) {
		case 'V':
			printf("%s\n", VERSION);
			exit(EXIT_SUCCESS);
			break;

		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
			break;

		default:
		case '?':
			return -1;
		}
	}

	opts->argc = argc;
	opts->argv = argv;
	return optind;
}

int uevent_event(char *action, char *devpath, void *data)
{
	(void)action;
	(void)devpath;
	(void)data;

	return 0;
}

int uevent_variable(char *variable, char *value, void *data)
{
	(void)data;
	if (strcmp(variable, "DEVNAME"))
		return 0;

	/* Spawn askfirst shell on tty2, tty3, tty4... */
	if (!__strncmp(value, "tty")) {
		if ((value[3] >= '2') && (value[3] <= '4') && (!value[4]))
			spawn("/bin/sh", sh, value);
	/* ... and on console */
	} else if (!strcmp(value, "console")) {
		spawn("/bin/sh", sh, value);
	}

	return 0;
}

int uevent_parse_line(char *line,
		      uevent_event_cb_t *evt_cb,
		      uevent_variable_cb_t *var_cb,
		      void *data)
{
	char *at, *equal;

	/* empty line? */
	if (*line == '\0')
		return 0;

	/* event? */
	at = strchr(line, '@');
	if (at) {
		char *action, *devpath;

		action = line;
		devpath = at + 1;
		*at = '\0';

		if (!evt_cb)
			return 0;

		return evt_cb(action, devpath, data);
	}

	/* variable? */
	equal = strchr(line, '=');
	if (equal) {
		char *variable, *value;

		variable = line;
		value = equal + 1;
		*equal = '\0';

		if (!var_cb)
			return 0;

		return var_cb(variable, value, data);
	}

	fprintf(stderr, "malformated event or variable: \"%s\"."
			" Must be either action@devpath,\n"
			"             or variable=value!\n", line);
	return 1;
}

int setup_signal(int fd, int signal)
{
	int flags;

	if (fcntl(fd, F_SETSIG, signal) == -1) {
		perror("fcntl");
		return -1;
	}

	if (fcntl(fd, F_SETOWN, getpid()) == -1) {
		perror("fcntl");
		return -1;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl");
		return -1;
	}

	flags |= (O_ASYNC | O_NONBLOCK | O_CLOEXEC);
	if (fcntl(fd, F_SETFL, flags) == -1) {
		perror("fcntl");
		return -1;
	}

	return 0;
}

int netlink_open(struct sockaddr_nl *addr, int signal)
{
	int fd;

	assert(addr);
	memset(addr, 0, sizeof(*addr));
	addr->nl_family = AF_NETLINK;
	addr->nl_pid = getpid();
	addr->nl_groups = NETLINK_KOBJECT_UEVENT;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
	if (fd == -1) {
		perror("socket");
		return -1;
	}

	if (bind(fd, (struct sockaddr *)addr, sizeof(*addr)) == -1) {
		perror("bind");
		goto error;
	}

	if (setup_signal(fd, signal) == -1) {
		__close(fd);
		goto error;
	}

	nl_fd = fd;
	return fd;

error:
	__close(fd);
	return -1;
}

int netlink_close(int fd)
{
	int ret;

	ret = close(fd);
	if (ret == -1)
		perror("close");

	nl_fd = -1;
	return ret;
}

ssize_t netlink_recv(int fd, struct sockaddr_nl *addr)
{
	char buf[UEVENT_BUFFER_SIZE];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = sizeof(buf),
	};
	struct msghdr msg = {
		.msg_name = addr,
		.msg_namelen = sizeof(*addr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	ssize_t len = 0;

	for (;;) {
		char *n, *s;
		ssize_t l;

		l = recvmsg(fd, &msg, 0);
		if (l == -1) {
			if (errno != EAGAIN) {
				perror("recvmsg");
				break;
			}

			break;
		} else if (!l) {
			break;
		}

		buf[l] = 0;
		s = buf;
		s += strlen(s) + 1;

		for (;;) {
			n = strchr(s, '\0');
			if (!n || n == s)
				break;

			if (uevent_parse_line(s, uevent_event, uevent_variable,
					      NULL))
				break;

			s = n + 1;
		}

		len += l;
	}

	return len;
}

int main(int argc, char * const argv[])
{
	static struct options_t options;
	struct sockaddr_nl addr;
	static sigset_t sigset;
	int fd, sig;

	int argi = parse_arguments(&options, argc, argv);
	if (argi < 0) {
		fprintf(stderr, "Error: %s: Invalid argument!\n",
				argv[optind-1]);
		exit(EXIT_FAILURE);
	} else if (argc - argi >= 1) {
		usage(stdout, argv[0]);
		fprintf(stderr, "Error: Too many arguments!\n");
		exit(EXIT_FAILURE);
	}

	if (sigemptyset(&sigset) == -1) {
		perror("sigemptyset");
		exit(EXIT_FAILURE);
	}

	sig = SIGTERM;
	if (sigaddset(&sigset, sig) == -1) {
		perror("sigaddset");
		exit(EXIT_FAILURE);
	}

	sig = SIGINT;
	if (sigaddset(&sigset, sig) == -1) {
		perror("sigaddset");
		exit(EXIT_FAILURE);
	}

	sig = SIGCHLD;
	if (sigaddset(&sigset, sig) == -1) {
		perror("sigaddset");
		exit(EXIT_FAILURE);
	}

	sig = SIGIO;
	if (sigaddset(&sigset, sig) == -1) {
		perror("perror");
		exit(EXIT_FAILURE);
	}

	if (sigprocmask(SIG_SETMASK, &sigset, NULL) == -1) {
		perror("perror");
		exit(EXIT_FAILURE);
	}

	fd = netlink_open(&addr, SIGIO);
	if (fd == -1)
		return EXIT_FAILURE;

	printf("tini started!\n");

	spawn("/etc/init.d/rcS", rcS, NULL);

	for (;;) {
		siginfo_t siginfo;
		sig = sigwaitinfo(&sigset, &siginfo);
		if (sig == -1) {
			if (errno == EINTR)
				continue;

			perror("sigwaitinfo");
			break;
		}

		/* Reap zombies */
		if (sig == SIGCHLD) {
			while (waitpid(-1, NULL, WNOHANG) > 0);
			continue;
		}

		/* Netlink uevent */
		if (sig == SIGIO) {
			netlink_recv(fd, &addr);
			continue;
		}

		/* Exit */
		if ((sig == SIGTERM) || (sig == SIGINT))
			break;
	}

	/* Reap zombies */
	while (waitpid(-1, NULL, WNOHANG) > 0);

	netlink_close(fd);
	fd = -1;

	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL) == -1)
		perror("sigprocmask");

	/* Reboot (Ctrl-Alt-Delete) */
	if (sig == SIGINT) {
		sync();
		if (reboot(RB_AUTOBOOT) == -1)
			perror("reboot");
		exit(EXIT_FAILURE);
	}

	/* Power off */
	printf("tini stopped!\n");

	sync();
	if (reboot(RB_POWER_OFF) == -1)
		perror("reboot");

	exit(EXIT_FAILURE);
}

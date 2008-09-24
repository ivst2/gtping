/** gtping/gtping.c
 *
 * GTP Ping
 *
 * By Thomas Habets <thomas@habets.pp.se> 2008
 *
 * Send GTP Ping and time the reply.
 *
 *
 */
/*
 *  Copyright (C) 2008 Thomas Habets <thomas@habets.pp.se>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

/* For those OSs that don't read RFC3493, even though their manpage
 * points to it. */
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif

#pragma pack(1)
struct GtpEcho {
	char flags;
	char msg;
	uint16_t len;	
	uint32_t teid;
	uint16_t seq;
	char npdu;
	char next;
};
#pragma pack()

#define DEFAULT_PORT 2123
#define DEFAULT_VERBOSE 0
#define DEFAULT_INTERVAL 1.0
struct Options {
	int port;
	int verbose;
	double interval;
	unsigned int count;
	const char *target;  /* what is on the cmdline */
	char *targetip;      /* IPv* address string */
};

static const double version = 0.12f;

static volatile int time_to_die = 0;
static unsigned int curSeq = 0;
#define SENDTIMES_SIZE 100
static double startTime;
static double sendTimes[SENDTIMES_SIZE];
static unsigned int totalTimeCount = 0;
static double totalTime = 0;
static double totalTimeSquared = 0;
static double totalMin = -1;
static double totalMax = -1;

/* from cmdline */
static const char *argv0 = 0;
static struct Options options = {
	port: DEFAULT_PORT,
	verbose: DEFAULT_VERBOSE,
	interval: DEFAULT_INTERVAL,
	count: 0,
	target: 0,
	targetip: 0,
};

static double gettimeofday_dbl();

/**
 *
 */
static void
sigint(int unused)
{
	unused = unused;
	time_to_die = 1;
}

/**
 * Create socket and "connect" it to target
 * allocates and sets options.targetip
 *
 * return fd, or <0 on error
 */
static int
setupSocket()
{
	int fd;
	int err;
	struct addrinfo *addrs = 0;
	struct addrinfo hints;
	char port[32];

	if (options.verbose > 1) {
		fprintf(stderr, "%s: setupSocket(%s)\n",
			argv0, options.target);
	}
	options.targetip = 0;
	
	snprintf(port, sizeof(port), "%u", options.port);

	if (!(options.targetip = malloc(NI_MAXHOST))) {
		err = errno;
		fprintf(stderr, "%s: malloc(NI_MAXHOST): %s\n",
			argv0, strerror(err));
		goto errout;
	}

	/* resolve to sockaddr */
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	if (0 > (err = getaddrinfo(options.target,
				   port,
				   &hints,
				   &addrs))) {
		if (err == EAI_NONAME) {
			fprintf(stderr, "%s: unknown host %s\n",
				argv0, options.target);
			return -1;
		}
		fprintf(stderr, "%s: getaddrinfo(): %s\n",
			argv0, gai_strerror(err));
		goto errout;
	}

	/* get ip address string options.targetip */
	if ((err = getnameinfo(addrs->ai_addr,
			       addrs->ai_addrlen,
			       options.targetip,
			       NI_MAXHOST,
			       NULL, 0,
			       NI_NUMERICHOST))) {
		err = errno;
		fprintf(stderr, "%s: getnameinfo(): %s\n",
			argv0,	gai_strerror(err));
		goto errout;
	}
	if (options.verbose) {
		fprintf(stderr, "%s: target=<%s> targetip=<%s>\n",
			argv0,
			options.target,
			options.targetip);
	}

	/* socket() */
	if (0 > (fd = socket(addrs->ai_family,
			     addrs->ai_socktype,
			     addrs->ai_protocol))) {
		err = errno;
		fprintf(stderr, "%s: socket(%d, %d, %d): %s\n",
			argv0,
			addrs->ai_family,
			addrs->ai_socktype,
			addrs->ai_protocol,
			strerror(err));
		goto errout;
	}

	/* connect() */
	if (connect(fd,
		    addrs->ai_addr,
		    addrs->ai_addrlen)) {
		err = errno;
		fprintf(stderr, "%s: connect(%d, ...): %s\n",
			argv0, fd, strerror(err));
		close(fd);
		goto errout;
	}

	freeaddrinfo(addrs);
	return fd;
 errout:
	if (addrs) {
		freeaddrinfo(addrs);
	}
	if (options.targetip) {
		free(options.targetip);
	}
	if (err > 0) {
		return -err;
	}
	return err;
}

/**
 * return 0 on succes, <0 on fail
 */
static int
sendEcho(int fd, int seq)
{
	int err;
	struct GtpEcho gtp;

	if (options.verbose > 1) {
		fprintf(stderr, "%s: sendEcho(%d, %d)\n", argv0, fd, seq);
	}

	if (options.verbose) {
		fprintf(stderr,	"%s: Sending GTP ping with seq=%d\n",
			argv0, curSeq);
	}

	memset(&gtp, 0, sizeof(struct GtpEcho));
	gtp.flags = 0x32;
	gtp.msg = 0x01;
	gtp.len = htons(4);
	gtp.teid = 0;
	gtp.seq = htons(seq);
	gtp.npdu = 0x00;
	gtp.next = 0x00;

	sendTimes[seq % SENDTIMES_SIZE] = gettimeofday_dbl();

	if (sizeof(struct GtpEcho) != send(fd, (void*)&gtp,
					   sizeof(struct GtpEcho), 0)) {
		err = errno;
		fprintf(stderr, "%s: send(%d, ...): %s\n",
			argv0, fd, strerror(errno));
		return -err;
	}
	return 0;
}

/**
 * return 0 on success/got reply, <0 on fail, >1 on success, no packet
 */
static int
recvEchoReply(int fd)
{
	int err;
	struct GtpEcho gtp;
	int n;
	double now;
	char lag[128];

	if (options.verbose > 1) {
		fprintf(stderr, "%s: recvEchoReply()\n", argv0);
	}

	now = gettimeofday_dbl();
	
	memset(&gtp, 0, sizeof(struct GtpEcho));

	if (0 > (n = recv(fd, (void*)&gtp, sizeof(struct GtpEcho), 0))) {
		switch(errno) {
		case ECONNREFUSED:
		case EINTR:
			printf("ICMP destination unreachable\n");
			return 1;
		default:
			err = errno;
			fprintf(stderr, "%s: recv(%d, ...): %s\n",
				argv0, fd, strerror(errno));
			return -err;
		}
	}
	if (gtp.msg != 0x02) {
		fprintf(stderr, "%s: Got non-EchoReply type of msg (%d)\n",
			argv0, gtp.msg);
		return 0;
	}

	if (curSeq - htons(gtp.seq) >= SENDTIMES_SIZE) {
		strcpy(lag, "Inf");
	} else {
		double lagf = (now-sendTimes[htons(gtp.seq)%SENDTIMES_SIZE]);
		snprintf(lag, sizeof(lag), "%.2f ms", 1000 * lagf);
		totalTime += lagf;
		totalTimeSquared += lagf * lagf;
		totalTimeCount++;
		if ((0 > totalMin) || (lagf < totalMin)) {
			totalMin = lagf;
		}
		if ((0 > totalMax) || (lagf > totalMax)) {
			totalMax = lagf;
		}
	}
	printf("%u bytes from %s: seq=%u time=%s\n",
	       n,
	       options.targetip,
	       htons(gtp.seq),
	       lag);
	return 0;
}

/**
 *
 */
static double
tv2dbl(const struct timeval *tv)
{
        return tv->tv_sec + tv->tv_usec / 1000000.0;
}

/**
 *
 */
static double
gettimeofday_dbl()
{
	struct timeval tv;
        if (gettimeofday(&tv, NULL)) {
		fprintf(stderr,"%s: gettimeofday(): %s\n",
			argv0,strerror(errno));
		return time(0);
	}
	return tv2dbl(&tv);
}

/**
 * return value is sent directly to return value of main()
 */
static int
mainloop(int fd)
{
	unsigned sent = 0;
	unsigned recvd = 0;
	double lastping = 0;
	double curping;

	if (options.verbose > 1) {
		fprintf(stderr, "%s: mainloop(%d)\n", argv0, fd);
	}

	startTime = gettimeofday_dbl();

	printf("GTPING %s (%s) %u bytes of data.\n",
	       options.target,
	       options.targetip,
	       (int)sizeof(struct GtpEcho));

	for(;!time_to_die;) {
		struct pollfd fds;
		double timewait;
		int n;

		curping = gettimeofday_dbl();
		if (curping > lastping + options.interval) {
			if (options.count && (curSeq == options.count)) {
				break;
			}
			if (0 > sendEcho(fd, curSeq++)) {
				return 1;
			}
			sent++;
			lastping = curping;
		}

		fds.fd = fd;
		fds.events = POLLIN;
		fds.revents = 0;
		
		timewait = (lastping + options.interval) - gettimeofday_dbl();
		if (timewait < 0) {
			timewait = 0;
		}
		switch ((n = poll(&fds, 1, (int)(timewait * 1000)))) {
		case 1: /* read ready */
			n = recvEchoReply(fd);
			if (!n) {
				recvd++;
			} else if (n > 0) {
				/* still ok, but no reply */
			} else {
				return 1;
			}
			break;
		case 0: /* timeout */
			break;
		case -1: /* error */
			switch (errno) {
			case EINTR:
			case EAGAIN:
				break;
			default:
				fprintf(stderr, "%s: poll([%d], 1, %d): %s\n",
					argv0,
					fd,
					(int)(timewait*1000),
					strerror(errno));
				exit(2);
			}
			break;
		default: /* can't happen */
			fprintf(stderr, "%s: poll() returned %d!\n", argv0, n);
			exit(2);
			break;
		}
			
	}
	printf("\n--- %s GTP ping statistics ---\n"
	       "%u packets transmitted, %u received, "
	       "%d%% packet loss, time %dms\n",
	       options.target,
	       sent, recvd,
	       (int)((100.0*(sent-recvd))/sent),
	       (int)(1000*(gettimeofday_dbl()-startTime)));
	if (totalTimeCount) {
		printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms",
		       1000*totalMin,
		       1000*(totalTime / totalTimeCount),
		       1000*totalMax,
		       1000*sqrt((totalTimeSquared -
				  (totalTime * totalTime)
				  /totalTimeCount)/totalTimeCount));
	}
	printf("\n");
	return recvd == 0;
}

/**
 *
 */
static void
usage(int err)
{
	printf("Usage: %s [ -hv ] [ -c <count> ] [ -p <port> ] "
	       "[ -w <time> ] <target>\n"
	       "\t-c <count>  Stop after sending count pings. "
	       "(default: 0=Infinite)\n"
	       "\t-h          Show this help text\n"
	       "\t-p <port>   GTP-C UDP port to ping (default: %d)\n"
	       "\t-v          Increase verbosity level (default: %d)\n"
	       "\t-w <time>   Time between pings (default: %.1f)\n",
	       argv0, DEFAULT_PORT, DEFAULT_VERBOSE, DEFAULT_INTERVAL);
	exit(err);
}

/**
 *
 */
int
main(int argc, char **argv)
{
	int fd;

	printf("GTPing %.2f, By Thomas Habets <thomas@habets.pp.se>\n",
	       version);

	argv0 = argv[0];
	{
		int c;
		while (-1 != (c = getopt(argc, argv, "c:hp:vw:"))) {
			switch(c) {
			case 'c':
				options.count = strtoul(optarg, 0, 0);
				break;
			case 'h':
				usage(0);
				break;
			case 'p':
				options.port = strtoul(optarg, 0, 0);
				break;
			case 'v':
				options.verbose++;
				break;
			case 'w':
				options.interval = atof(optarg);
				break;
			case '?':
			default:
				usage(2);
			}
		}
	}

	if (optind + 1 != argc) {
		usage(2);
	}

	options.target = argv[optind];

	if (SIG_ERR == signal(SIGINT, sigint)) {
		fprintf(stderr, "%s: signal(SIGINT, ...): %s\n",
			argv0, strerror(errno));
		return 1;
	}

	if (0 > (fd = setupSocket())) {
		return 1;
	}

	return mainloop(fd);
}

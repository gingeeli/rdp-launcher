/* rdp-launcher: is anything listening at addr:port? (non-blocking connect) */
#include "rl.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int rl_probe(const char *addr, int port, int timeout_ms)
{
	struct addrinfo hints, *res = NULL, *ai;
	struct pollfd pfd;
	char service[16];
	socklen_t len;
	int fd, flags, err, result = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof service, "%d", port);
	if (getaddrinfo(addr, service, &hints, &res) != 0 || !res)
		return 2;

	for (ai = res; ai && result != 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
			close(fd);
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			result = 0;
		} else if (errno == EINPROGRESS) {
			pfd.fd = fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			if (poll(&pfd, 1, timeout_ms) == 1) {
				err = 0;
				len = sizeof err;
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
					result = 0;
			}
		}
		close(fd);
	}
	freeaddrinfo(res);
	return result;
}

int rl_pick_addr(const struct rl_host *h, int timeout_ms)
{
	int i;

	for (i = 0; i < h->naddrs; i++)
		if (rl_probe(h->addrs[i], h->port, timeout_ms) == 0)
			return i;
	return -1;
}

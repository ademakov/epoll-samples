#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sys/epoll.h>

#include <memory>
#include <queue>
#include <thread>

#include "fd_queue.h"

#define NEVENTS		(128)

#define NPINGERS	(4)
#define NPONGERS	(3)
#define NPOLLERS	(1)

#define NPSOCKETS	(100)
#define NMESSAGES	(25000)

#define QUEUE_SIZE	(1u << 13)

#define PINGER_TIMEOUT	1
#define POLLER_TIMEOUT	1

struct connection
{
	int fd;
	int count;

	connection() : fd(-1), count(0)
	{
	}

	~connection()
	{
		if (fd >= 0)
			close(fd);
	}
};

void
pong(fd_queue<QUEUE_SIZE> &queue)
{
	for (;;) {
		int fd = queue.pop();
		if (fd <= 0) {
			if (fd == 0)
				break;
			close(-fd);
			continue;
		}

		char buf[4];
		ssize_t len = read(fd, buf, 4);
		if (len == 0) {
			printf("oops, closed %d\n", fd);
			continue;
		}

		if (len < 0)
			error(1, errno, "consumer read() failed");
		if (len != 4 || memcmp(buf, "ping", 4) != 0)
			error(1, 0, "consumer read bad data (%zd)", len);

		if (write(fd, "pong", 4) != 4)
			error(1, errno, "consumer write() failed");
	}
}

void
ping(std::vector<sockaddr_in> &addresses)
{
	int efd = epoll_create(EPOLL_CLOEXEC);
	if (efd < 0)
		error(1, errno, "epoll_create() failed");

	std::unique_ptr<connection[]> conns(new connection[NPSOCKETS]);
	for (int i = 0; i < NPSOCKETS; i++) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			error(1, errno, "socket() failed");

		conns[i].fd = fd;

		sockaddr_in &addr = addresses[i % addresses.size()];
		if (connect(fd, (struct sockaddr *) &addr, sizeof addr) < 0)
			error(1, errno, "listen() failed");

		struct epoll_event ee;
		ee.events = EPOLLIN;
		ee.data.ptr = &conns[i];
		if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ee) < 0)
			error(1, errno, "epoll_ctl() failed");
	}

	std::queue<connection*> queue;
	for (int i = 0; i < NPSOCKETS; i++)
		queue.push(&conns[i]);

	int total = 0;
	while (total < (NPSOCKETS * NMESSAGES)) {
		while (!queue.empty()) {
			connection *conn = queue.front();
			queue.pop();

			if (write(conn->fd, "ping", 4) != 4)
				error(1, errno, "producer write() failed");
		}

		struct epoll_event events[NEVENTS];
		int n = epoll_wait(efd, events, NEVENTS, PINGER_TIMEOUT);
		if (n < 0)
			error(1, errno, "epoll_wait() failed");

		for (int i = 0; i < n; i++) {
			struct epoll_event &e = events[i];
			connection *conn = static_cast<connection *>(e.data.ptr);

			char buf[4];
			ssize_t len = read(conn->fd, buf, 4);
			if (len < 0)
				error(1, errno, "producer read() failed");
			if (len != 4 || memcmp(buf, "pong", 4) != 0)
				error(1, 0, "producer read bad data");

			conn->count++;
			if (conn->count < NMESSAGES)
				queue.push(conn);
			total++;
		}
	}
}

struct poll
{
	int efd = -1;
	int sfd = -1;
	::sockaddr_in address;

	fd_queue<QUEUE_SIZE> queue;
	std::uint64_t overflows = 0;
	std::uint64_t stats[NEVENTS + 1];

	poll()
	{
		sfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sfd < 0)
			error(1, errno, "socket() failed");

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(0);

		if (bind(sfd, (struct sockaddr *) &address, sizeof address) < 0)
			error(1, errno, "bind() failed");
		if (listen(sfd, 128) < 0)
			error(1, errno, "listen() failed");

		{
			socklen_t addrsz = sizeof address;
			if (getsockname(sfd, (struct sockaddr *) &address, &addrsz))
				error(1, errno, "getsockname() failed");
		}
		{
			int flags = fcntl(sfd, F_GETFL, 0);
			if (flags < 0)
				error(1, errno, "fcntl(..., F_GETFL, ...) failed");
			if (fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
				error(1, errno, "fcntl(..., F_SETFL, ...) failed");
		}

		efd = epoll_create(EPOLL_CLOEXEC);
		if (efd < 0)
			error(1, errno, "epoll_create() failed");

		struct epoll_event ee;
		ee.events = EPOLLIN;
		ee.data.fd = sfd;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ee) < 0)
			error(1, errno, "epoll_ctl() failed");

		memset(stats, 0, (NEVENTS + 1) * sizeof stats[0]);
	}

	~poll()
	{
		if (efd >= 0)
			close(efd);
		if (sfd >= 0)
			close(sfd);
	}

	void run(int nsockets)
	{
		int closed = 0;
		struct epoll_event ee;
		struct epoll_event events[NEVENTS];
		do {
			int n = epoll_wait(efd, events, NEVENTS, POLLER_TIMEOUT);
			if (n < 0)
				error(1, errno, "epoll_wait() failed");
			stats[n]++;

			for (int i = 0; i < n; i++) {
				struct epoll_event &e = events[i];
				if (e.data.fd == sfd) {
					for (;;) {
						int fd = accept(sfd, NULL, NULL);
						if (fd < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							error(1, errno, "accept() failed");
						}

						ee.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
						ee.data.fd = fd;
						if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ee) < 0)
							error(1, errno, "epoll_ctl() failed");
					}
				} else if ((e.events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
					if (epoll_ctl(efd, EPOLL_CTL_DEL, e.data.fd, &ee) < 0)
						error(1, errno, "epoll_ctl() failed");
					queue.push(-e.data.fd);
					++closed;
				} else {
					overflows += queue.push(e.data.fd);
				}
			}
		} while (closed < nsockets);
	}
};

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	std::thread pollers[NPOLLERS];
	std::vector<poll> polls(NPOLLERS);
	for (int i = 0; i < NPOLLERS; i++) {
		int nsockets = (NPSOCKETS / NPOLLERS) * NPINGERS;
		if (i < (NPSOCKETS % NPOLLERS))
			nsockets += NPINGERS;
		pollers[i] = std::thread(&poll::run, &polls[i], nsockets);
	}

	std::thread pongers[NPONGERS];
	for (int i = 0; i < NPONGERS; i++) {
		static_assert(NPONGERS >= NPOLLERS);
		pongers[i] = std::thread(pong, std::ref(polls[i % NPOLLERS].queue));
	}

	std::vector<::sockaddr_in> addresses(NPOLLERS);
	for (int i = 0; i < NPOLLERS; i++)
		addresses[i] = polls[i].address;
	for (int i = 0; i < NPINGERS; i++)
		std::thread(ping, std::ref(addresses)).detach();

	for (int i = 0; i < NPOLLERS; i++)
		pollers[i].join();
	for (int i = 0; i < NPONGERS; i++)
		polls[i % NPOLLERS].queue.push(0);
	for (int i = 0; i < NPONGERS; i++)
		pongers[i].join();

	for (int i = 0; i < NPOLLERS; i++) {
		printf("queue overflows: %lu\n", polls[i].overflows);
		printf("epoll stats: [");
		for (int j = 0; j <= NEVENTS; j++)
			printf(" %d=%lu,", j, polls[i].stats[j]);
		printf(" ]\n\n");
	}

	return 0;
}

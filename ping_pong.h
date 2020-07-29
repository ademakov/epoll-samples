#pragma once

#include "fd_queue.h"

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <queue>
#include <thread>

#define NEVENTS		(128)

#define NPINGERS	(4)
#define NPONGERS	(3)
#define NPOLLERS	(1)

#define NPSOCKETS	(100)
#define NMESSAGES	(25000)

#define QUEUE_SIZE	(1u << 13)

#define PINGER_TIMEOUT	1
#define POLLER_TIMEOUT	1

union address
{
	::sockaddr_in in;
	::sockaddr_un un;
};

struct ping
{
	struct connection
	{
		int fd = -1;
		int count = 0;

		~connection()
		{
			if (fd >= 0)
				close(fd);
		}
	};

	int efd = -1;

	~ping()
	{
		if (efd >= 0)
			close(efd);
	}

	static void run(std::function<int(const address&)> connect, const std::vector<address>& addresses)
	{
		ping ping;
		ping.efd = epoll_create(EPOLL_CLOEXEC);
		if (ping.efd < 0)
			error(1, errno, "epoll_create() failed");

		std::queue<connection*> queue;
		std::unique_ptr<ping::connection[]> conns(new ping::connection[NPSOCKETS]);
		for (int i = 0; i < NPSOCKETS; i++) {
			int fd = connect(addresses[i % addresses.size()]);
			conns[i].fd = fd;

			struct epoll_event ee;
			ee.events = EPOLLIN;
			ee.data.ptr = &conns[i];
			if (epoll_ctl(ping.efd, EPOLL_CTL_ADD, fd, &ee) < 0)
				error(1, errno, "epoll_ctl() failed 1");

			queue.push(&conns[i]);
		}

		int total = 0;
		while (total < (NPSOCKETS * NMESSAGES)) {
			while (!queue.empty()) {
				connection *conn = queue.front();
				queue.pop();

				if (write(conn->fd, "ping", 4) != 4)
					error(1, errno, "producer write() failed");
			}

			struct epoll_event events[NEVENTS];
			int n = epoll_wait(ping.efd, events, NEVENTS, PINGER_TIMEOUT);
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
};

struct pong
{
	static void
	run(fd_queue<QUEUE_SIZE> &queue)
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

	struct poll
	{
		int efd = -1;
		int sfd = -1;

		fd_queue<QUEUE_SIZE> queue;
		std::uint64_t overflows = 0;
		std::uint64_t stats[NEVENTS + 1];

		poll()
		{
			memset(stats, 0, (NEVENTS + 1) * sizeof stats[0]);
		}

		~poll()
		{
			if (efd >= 0)
				close(efd);
			if (sfd >= 0)
				close(sfd);
		}

		void init(std::function<int(address&)> setup, address& addr)
		{
			sfd = setup(addr);

			if (listen(sfd, 128) < 0)
				error(1, errno, "listen() failed");

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
};

struct ping_pong
{
	std::thread pollers[NPOLLERS];
	std::thread pongers[NPONGERS];

	void run(std::function<int(address&)> setup, std::function<int(const address&)> connect)
	{
		std::vector<pong::poll> polls(NPOLLERS);
		std::vector<address> addresses(NPOLLERS);
		for (int i = 0; i < NPOLLERS; i++) {
			polls[i].init(setup, addresses[i]);

			int nsockets = (NPSOCKETS / NPOLLERS) * NPINGERS;
			if (i < (NPSOCKETS % NPOLLERS))
				nsockets += NPINGERS;
			pollers[i] = std::thread(&pong::poll::run, &polls[i], nsockets);
		}

		for (int i = 0; i < NPONGERS; i++) {
			static_assert(NPONGERS >= NPOLLERS);
			pongers[i] = std::thread(&pong::run, std::ref(polls[i % NPOLLERS].queue));
		}
		for (int i = 0; i < NPINGERS; i++)
			std::thread(&ping::run, connect, std::cref(addresses)).detach();

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
	}
};

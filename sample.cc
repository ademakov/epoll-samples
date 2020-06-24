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

#include <atomic>
#include <memory>
#include <queue>
#include <thread>

#define NEVENTS		(128)

#define NPRODUCERS	(3)
#define NCONSUMERS	(3)

#define NPSOCKETS	(100)
#define NMESSAGES	(10000)

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

struct queue
{
	static constexpr size_t SIZE = 2048;

	struct slot
	{
		alignas(64) std::atomic<uint32_t> lock;
		int data;
	};

	alignas(64) std::atomic<uint32_t> head;
	alignas(64) std::atomic<uint32_t> tail;
	alignas(64) std::unique_ptr<slot[]> slots;

	queue()	: head(0), tail(0), slots(new slot[SIZE])
	{
		for (size_t i = 0; i < SIZE; i++)
			slots[i].lock.store(i, std::memory_order_relaxed);
	}

	size_t push(int data)
	{
		const size_t n = tail.fetch_add(1, std::memory_order_relaxed);
		slot &s = slots[n & (SIZE - 1)];

		size_t overflows = 0;
		while (s.lock.load(std::memory_order_acquire) != n) {
			++overflows;
			__builtin_ia32_pause();
		}

		s.data = data;
		s.lock.store(n + 1, std::memory_order_release);

		return overflows;
	}

	int pop()
	{
		const size_t n = head.fetch_add(1, std::memory_order_relaxed);
		slot &s = slots[n & (SIZE - 1)];

		while (s.lock.load(std::memory_order_acquire) != (n + 1))
			__builtin_ia32_pause();

		int data = s.data;
		s.lock.store(n + SIZE, std::memory_order_release);

		return data;
	}
};

void
consumer(queue &queue)
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
producer(struct sockaddr_in &addr)
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
		int n = epoll_wait(efd, events, NEVENTS, 0);
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

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0)
		error(1, errno, "socket() failed");

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(0);

	if (bind(sfd, (struct sockaddr *) &addr, sizeof addr) < 0)
		error(1, errno, "bind() failed");
	if (listen(sfd, 128) < 0)
		error(1, errno, "listen() failed");

	{
		socklen_t addrsz = sizeof addr;
		if (getsockname(sfd, (struct sockaddr *) &addr, &addrsz))
			error(1, errno, "getsockname() failed");
	}
	{
		int flags = fcntl(sfd, F_GETFL, 0);
		if (flags < 0)
			error(1, errno, "fcntl(..., F_GETFL, ...) failed");
		if (fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0)
			error(1, errno, "fcntl(..., F_SETFL, ...) failed");
	}

	int efd = epoll_create(EPOLL_CLOEXEC);
	if (efd < 0)
		error(1, errno, "epoll_create() failed");

	struct epoll_event ee;
	ee.events = EPOLLIN;
	ee.data.fd = sfd;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ee) < 0)
		error(1, errno, "epoll_ctl() failed");

	for (int p = 0; p < NPRODUCERS; p++)
		std::thread(producer, std::ref(addr)).detach();

	queue queue;
	std::thread consumers[NCONSUMERS];
	for (int c = 0; c < NCONSUMERS; c++)
		consumers[c] = std::thread(consumer, std::ref(queue));

	int closed = 0;
	size_t overflows = 0;
	int stats[NEVENTS + 1];
	memset(stats, 0, (NEVENTS + 1) * sizeof stats[0]);
	struct epoll_event events[NEVENTS];
	do {
		int n = epoll_wait(efd, events, NEVENTS, 0);
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
	} while (closed < NPRODUCERS);

	printf("queue overflows: %zu\n", overflows);
	printf("epoll stats: [");
	for (int i = 0; i <= NEVENTS; i++)
		printf(" %d=%d,", i, stats[i]);
	printf(" ]\n");

	for (int c = 0; c < NCONSUMERS; c++)
		queue.push(0);
	for (int c = 0; c < NCONSUMERS; c++)
		consumers[c].join();

	return 0;
}

#include "ping_pong.h"

#include <signal.h>

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	queued_ping_pong pp;
	pp.run(
		[] (address& addr) -> int {
			int fd = socket(PF_INET, SOCK_STREAM, 0);
			if (fd < 0)
				error(1, errno, "socket() failed");

			memset(&addr, 0, sizeof addr);
			addr.in.sin_family = AF_INET;
			addr.in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.in.sin_port = htons(0);
			if (bind(fd, (::sockaddr *) &addr.in, sizeof addr.in) < 0)
				error(1, errno, "bind() failed");

			{
				socklen_t addrsz = sizeof addr.in;
				if (getsockname(fd, (::sockaddr *) &addr.in, &addrsz))
					error(1, errno, "getsockname() failed");
			}

			return fd;
		},
		[] (const address& addr) -> int {
			int fd = socket(PF_INET, SOCK_STREAM, 0);
			if (fd < 0)
				error(1, errno, "socket() failed");

			if (::connect(fd, (::sockaddr *) &addr.in, sizeof addr.in) < 0)
				error(1, errno, "connect() failed");

			return fd;
		}
	);

	return 0;
}

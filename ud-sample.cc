#include "ping_pong.h"

#include <signal.h>

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	ping_pong pp;
	pp.run(
		[] (address& addr) -> int {
			int fd = socket(PF_UNIX, SOCK_STREAM, 0);
			if (fd < 0)
				error(1, errno, "socket() failed");

			memset(&addr, 0, sizeof addr);
			addr.un.sun_family = AF_UNIX;
			// Use Linux-specific autobind of a unix-domain socket.
			if (bind(fd, (::sockaddr *) &addr.un, sizeof(sa_family_t)) < 0)
				error(1, errno, "bind() failed");

			{
				socklen_t addrsz = sizeof(sa_family_t) + 6;
				if (getsockname(fd, (::sockaddr *) &addr.un, &addrsz))
					error(1, errno, "getsockname() failed");
			}

			return fd;
		},
		[] (const address& addr) -> int {
			int fd = socket(PF_UNIX, SOCK_STREAM, 0);
			if (fd < 0)
				error(1, errno, "socket() failed");

			if (::connect(fd, (::sockaddr *) &addr.un, sizeof(sa_family_t) + 6) < 0)
				error(1, errno, "connect() failed");

			return fd;
		}
	);

	return 0;
}

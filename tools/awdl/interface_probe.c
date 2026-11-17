/* Read the local Darwin interface state. Print neither interface addresses
 * nor peer information. A link-local address alone cannot establish AWDL. */
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* XNU 8796.121.2, bsd/sys/sockio.h and bsd/net/if_private.h. */
#ifndef SIOCGIFEFLAGS
#define SIOCGIFEFLAGS _IOWR('i', 142, struct ifreq)
#endif
#ifndef IFEF_AWDL
#define IFEF_AWDL 0x00100000u
#endif

static void
report(int sock, const struct ifaddrs *list, const char *name)
{
	unsigned index = if_nametoindex(name), linklocal = 0, ipv6 = 0, flags = 0;
	for (const struct ifaddrs *a = list; a; a = a->ifa_next) {
		if (!a->ifa_name || strcmp(name, a->ifa_name)) continue;
		flags |= a->ifa_flags;
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET6) continue;
		const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)a->ifa_addr;
		ipv6++;
		if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) linklocal++;
	}
	int marked = -1, error = 0;
	if (index) {
		struct ifreq query = {0};
		strlcpy(query.ifr_name, name, sizeof(query.ifr_name));
		if (ioctl(sock, SIOCGIFEFLAGS, &query) == 0) {
			uint32_t extended = 0;
			memcpy(&extended, &query.ifr_ifru, sizeof(extended));
			marked = (extended & IFEF_AWDL) != 0;
		} else {
			error = errno;
		}
	}
	printf("awdl-probe: if=%s present=%u up=%u running=%u multicast=%u "
	    "ipv6=%u linklocal=%u awdl_flag=%d flag_errno=%d\n",
	    name, index != 0, !!(flags & IFF_UP), !!(flags & IFF_RUNNING),
	    !!(flags & IFF_MULTICAST), ipv6, linklocal, marked, error);
}

int
main(void)
{
	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) < 0) { perror("getifaddrs"); return 1; }
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) { perror("socket"); freeifaddrs(list); return 1; }
	report(sock, list, "en0");
	report(sock, list, "awdl0");
	close(sock);
	freeifaddrs(list);
	return 0;
}

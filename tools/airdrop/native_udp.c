/* One scoped iOS UDP request to a designated link-local peer. Success requires
 * the matching reply on the bound interface. Errors are positive Darwin errno. */
#define __APPLE_USE_RFC_3542 1
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "udp_wire.h"

static int64_t udp_clock_ms(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t udp_elapsed_ms(int64_t start)
{
    int64_t now = udp_clock_ms(CLOCK_MONOTONIC);
    return now >= start ? now - start : -1;
}

static int udp_bind(int fd, unsigned index, const struct in6_addr *local)
{
    int flags = fcntl(fd, F_GETFL);
    int on = 1;
    struct sockaddr_in6 source = {.sin6_len = sizeof(source), .sin6_family = AF_INET6,
                                   .sin6_scope_id = index, .sin6_addr = *local};
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, sizeof(index)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0 ||
        bind(fd, (const struct sockaddr *)&source, sizeof(source)) < 0)
        return errno;

    unsigned bound_if = 0;
    socklen_t if_size = sizeof(bound_if);
    int if_errno = getsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &bound_if, &if_size) < 0 ? errno : 0;
    int if_match = !if_errno && if_size == sizeof(bound_if) && bound_if == index;
    struct sockaddr_in6 bound = {0};
    socklen_t size = sizeof(bound);
    int source_errno = getsockname(fd, (struct sockaddr *)&bound, &size) < 0 ? errno : 0;
    int source_match = !source_errno && size >= sizeof(bound) && bound.sin6_family == AF_INET6 &&
                       IN6_ARE_ADDR_EQUAL(&bound.sin6_addr, local);
    int scope_ok = source_match && (!bound.sin6_scope_id || bound.sin6_scope_id == index);
    int port_bound = source_match && bound.sin6_port != 0;
    printf("awdl-native: udp_binding bound_if_match=%d bound_if_errno=%d source_match=%d "
           "source_errno=%d scope_ok=%d port_bound=%d\n",
           if_match, if_errno, source_match, source_errno, scope_ok, port_bound);
    fflush(stdout);
    if (if_errno) return if_errno;
    if (source_errno) return source_errno;
    if (!if_match) return ENXIO;
    if (!scope_ok || !port_bound) return EADDRNOTAVAIL;
    return 0;
}

static int udp_destination_matches(struct msghdr *msg, unsigned index, const struct in6_addr *local)
{
    for (struct cmsghdr *h = CMSG_FIRSTHDR(msg); h; h = CMSG_NXTHDR(msg, h)) {
        if (h->cmsg_level == IPPROTO_IPV6 && h->cmsg_type == IPV6_PKTINFO &&
            h->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(h), sizeof(info));
            return info.ipi6_ifindex == index && IN6_ARE_ADDR_EQUAL(&info.ipi6_addr, local);
        }
    }
    return 0;
}

struct udp_receipt {
    uint64_t calls, datagrams;
    ssize_t last_rc;
    int last_errno;
    char reply_digest[65];
};

static int udp_receive(int fd, unsigned index, const struct in6_addr *local,
                       const struct in6_addr *peer, const uint8_t token[16],
                       int64_t deadline, struct udp_receipt *receipt)
{
    for (;;) {
        int64_t now = udp_clock_ms(CLOCK_MONOTONIC);
        if (now < 0) return errno;
        if (now >= deadline) return ETIMEDOUT;
        int64_t remaining = deadline - now;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (!ready) continue;
        if (pfd.revents & POLLNVAL) return EBADF;

        struct hk_udp_message reply;
        struct sockaddr_in6 source = {0};
        struct iovec iov = {.iov_base = &reply, .iov_len = sizeof(reply)};
        union {
            struct cmsghdr align;
            uint8_t bytes[CMSG_SPACE(sizeof(struct in6_pktinfo))];
        } control;
        struct msghdr msg = {.msg_name = &source, .msg_namelen = sizeof(source),
                             .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes,
                             .msg_controllen = sizeof(control.bytes)};
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
        receipt->last_errno = n < 0 ? errno : 0;
        receipt->last_rc = n;
        receipt->calls++;
        if (n < 0) {
            int error = receipt->last_errno;
            if (error == EINTR) continue;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                if (!(pfd.revents & (POLLERR | POLLHUP))) continue;
                int socket_error = 0;
                socklen_t len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0) return errno;
                return socket_error ? socket_error : ENOTCONN;
            }
            return error;
        }
        receipt->datagrams++;
        if (n == sizeof(reply) && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) &&
            msg.msg_namelen >= sizeof(source) && source.sin6_family == AF_INET6 &&
            source.sin6_port == htons(HK_UDP_PORT) &&
            (!source.sin6_scope_id || source.sin6_scope_id == index) &&
            IN6_ARE_ADDR_EQUAL(&source.sin6_addr, peer) &&
            udp_destination_matches(&msg, index, local) && hk_udp_matches(&reply, HK_UDP_REPLY, token)) {
            hk_udp_digest(&reply, receipt->reply_digest);
            return 0;
        }
    }
}

int hk_native_udp6(const char *ifname, const struct in6_addr *local, const struct in6_addr *peer,
                   const uint8_t token[16], unsigned timeout_ms)
{
    int64_t start = udp_clock_ms(CLOCK_MONOTONIC);
    if (start < 0) return errno;
    if (!ifname || !ifname[0] || strnlen(ifname, IFNAMSIZ) == IFNAMSIZ || !local ||
        !peer || !token || !timeout_ms || !IN6_IS_ADDR_LINKLOCAL(local) ||
        !IN6_IS_ADDR_LINKLOCAL(peer) || IN6_ARE_ADDR_EQUAL(local, peer))
        return EINVAL;
    unsigned index = if_nametoindex(ifname);
    if (!index) return errno ? errno : ENXIO;
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return errno;
    int error = udp_bind(fd, index, local);
    struct udp_receipt receipt = {.last_rc = -1};
    int attempted = 0, waited = 0;
    if (!error) {
        struct sockaddr_in6 to = {.sin6_len = sizeof(to), .sin6_family = AF_INET6,
                                  .sin6_addr = *peer, .sin6_scope_id = index,
                                  .sin6_port = htons(HK_UDP_PORT)};
        struct hk_udp_message request;
        hk_udp_make(&request, HK_UDP_REQUEST, token);
        char request_digest[65];
        hk_udp_digest(&request, request_digest);
        int64_t began = udp_elapsed_ms(start);
        int64_t uptime = udp_clock_ms(CLOCK_UPTIME_RAW);
        attempted = 1;
        ssize_t n = sendto(fd, &request, sizeof(request), 0, (struct sockaddr *)&to, sizeof(to));
        int send_errno = n < 0 ? errno : 0;
        printf("awdl-native: udp_send start_relative_ms=%" PRId64 " start_uptime_ms=%" PRId64
               " return_relative_ms=%" PRId64 " return_uptime_ms=%" PRId64
               " sendto_rc=%zd bytes=%zd errno=%d request_sha256=%s\n",
               began, uptime, udp_elapsed_ms(start), udp_clock_ms(CLOCK_UPTIME_RAW),
               n, n < 0 ? (ssize_t)0 : n, send_errno, request_digest);
        fflush(stdout);
        if (send_errno) error = send_errno;
        else if (n != sizeof(request)) error = EIO;
        else {
            waited = 1;
            error = udp_receive(fd, index, local, peer, token, start + timeout_ms, &receipt);
        }
    }
    printf("awdl-native: udp_result send_attempted=%d waited=%d recv_calls=%" PRIu64
           " recv_datagrams=%" PRIu64 " last_recv_rc=%zd last_recv_errno=%d reply_verified=%d"
           " result_errno=%d relative_ms=%" PRId64 " uptime_ms=%" PRId64 " reply_sha256=%s\n",
           attempted, waited, receipt.calls, receipt.datagrams, receipt.last_rc, receipt.last_errno,
           waited && !error, error, udp_elapsed_ms(start), udp_clock_ms(CLOCK_UPTIME_RAW),
           receipt.reply_digest[0] ? receipt.reply_digest : "unobserved");
    fflush(stdout);
    close(fd);
    return error;
}

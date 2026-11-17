/* A single scoped ICMPv6 datagram probe. Returns 0 only for its own reply,
 * otherwise a positive errno. The kernel generates and verifies checksums. */
#define __APPLE_USE_RFC_3542 1
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct native_echo_message {
    struct icmp6_hdr header;
    uint8_t nonce[16];
};

_Static_assert(sizeof(struct native_echo_message) == 24, "fixed ICMPv6 echo");

static int echo_now(uint64_t *nanoseconds)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return errno;
    *nanoseconds = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return 0;
}

struct echo_time {
    int64_t relative_ms;
    int64_t uptime_ms;
};

static struct echo_time echo_time_at(uint64_t entry_ns)
{
    struct echo_time t = {.relative_ms = -1, .uptime_ms = -1};
    uint64_t now;
    if (echo_now(&now) == 0 && now >= entry_ns)
        t.relative_ms = (int64_t)((now - entry_ns) / 1000000u);
    struct timespec uptime;
    if (clock_gettime(CLOCK_UPTIME_RAW, &uptime) == 0)
        t.uptime_ms = (int64_t)uptime.tv_sec * 1000 + uptime.tv_nsec / 1000000;
    return t;
}

static int echo_binding(int fd, unsigned index, const struct in6_addr *local)
{
    unsigned actual = 0;
    socklen_t option_size = sizeof(actual);
    int if_error = getsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &actual, &option_size) < 0 ? errno : 0;
    int if_match = !if_error && option_size == sizeof(actual) && actual == index;
    struct sockaddr_in6 bound = {0};
    socklen_t address_size = sizeof(bound);
    int source_error = getsockname(fd, (struct sockaddr *)&bound, &address_size) < 0 ? errno : 0;
    int source_match = !source_error && address_size >= sizeof(bound) &&
                       bound.sin6_family == AF_INET6 && IN6_ARE_ADDR_EQUAL(&bound.sin6_addr, local);
    int scope_present = source_match && bound.sin6_scope_id != 0;
    int scope_match = scope_present && bound.sin6_scope_id == index;
    printf("awdl-native: echo_binding bound_if_match=%d bound_if_errno=%d "
           "source_match=%d source_errno=%d scope_present=%d scope_match=%d\n",
           if_match, if_error, source_match, source_error, scope_present, scope_match);
    fflush(stdout);
    if (if_error) return if_error;
    if (source_error) return source_error;
    if (!if_match) return ENXIO;
    if (!source_match || (scope_present && !scope_match)) return EADDRNOTAVAIL;
    return 0;
}

struct echo_receive {
    uint64_t calls, datagrams;
    ssize_t last_rc;
    int last_errno;
};

static int echo_received_on(struct msghdr *msg, unsigned index, const struct in6_addr *local)
{
    for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c; c = CMSG_NXTHDR(msg, c)) {
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO &&
            c->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(c), sizeof(info));
            return info.ipi6_ifindex == index && IN6_ARE_ADDR_EQUAL(&info.ipi6_addr, local);
        }
    }
    return 0;
}

static int echo_wait(int fd, unsigned index, const struct in6_addr *local,
                     const struct in6_addr *peer, const struct native_echo_message *request,
                     uint64_t deadline, struct echo_receive *receive)
{
    for (;;) {
        uint64_t now;
        int error = echo_now(&now);
        if (error) return error;
        if (now >= deadline) return ETIMEDOUT;
        uint64_t remaining_ms = (deadline - now + 999999u) / 1000000u;
        int wait_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pollfd, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        if (!ready) continue;
        if (pollfd.revents & POLLNVAL) return EBADF;
        struct native_echo_message reply;
        struct sockaddr_in6 source = {0};
        union {
            struct cmsghdr aligned;
            uint8_t bytes[CMSG_SPACE(sizeof(struct in6_pktinfo))];
        } control;
        struct iovec iov = {.iov_base = &reply, .iov_len = sizeof(reply)};
        struct msghdr msg = {.msg_name = &source, .msg_namelen = sizeof(source),
            .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.bytes,
            .msg_controllen = sizeof(control.bytes)};
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
        receive->last_errno = n < 0 ? errno : 0;
        receive->last_rc = n;
        receive->calls++;
        if (n >= 0) receive->datagrams++;
        if (n < 0) {
            int receive_error = receive->last_errno;
            if (receive_error == EINTR) continue;
            if (receive_error == EAGAIN || receive_error == EWOULDBLOCK) {
                if (!(pollfd.revents & (POLLERR | POLLHUP))) continue;
                int socket_error = 0;
                socklen_t len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0) return errno;
                return socket_error ? socket_error : ENOTCONN;
            }
            return receive_error;
        }
        if (n == sizeof(reply) && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
            msg.msg_namelen >= sizeof(source) && source.sin6_family == AF_INET6 &&
            (!source.sin6_scope_id || source.sin6_scope_id == index) &&
            IN6_ARE_ADDR_EQUAL(&source.sin6_addr, peer) &&
            reply.header.icmp6_type == ICMP6_ECHO_REPLY && reply.header.icmp6_code == 0 &&
            reply.header.icmp6_id == request->header.icmp6_id &&
            reply.header.icmp6_seq == request->header.icmp6_seq &&
            memcmp(reply.nonce, request->nonce, sizeof(reply.nonce)) == 0 &&
            echo_received_on(&msg, index, local))
            return 0;
    }
}

int hk_native_echo6(const char *ifname, const struct in6_addr *local, const struct in6_addr *peer,
                    unsigned timeout_ms)
{
    uint64_t entry_ns;
    int error = echo_now(&entry_ns);
    if (error) return error;
    if (!ifname || !ifname[0] || strnlen(ifname, IFNAMSIZ) == IFNAMSIZ ||
        !local || !peer || !timeout_ms || !IN6_IS_ADDR_LINKLOCAL(local) ||
        !IN6_IS_ADDR_LINKLOCAL(peer) || IN6_ARE_ADDR_EQUAL(local, peer))
        return EINVAL;
    unsigned index = if_nametoindex(ifname);
    if (!index) return errno ? errno : ENXIO;
    uint64_t deadline = entry_ns + (uint64_t)timeout_ms * 1000000u;
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    if (fd < 0) return errno;

    int flags = fcntl(fd, F_GETFL);
    int on = 1;
    struct sockaddr_in6 address = {.sin6_len = sizeof(address), .sin6_family = AF_INET6,
                                   .sin6_scope_id = index, .sin6_addr = *local};
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, sizeof(index)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        error = errno;
    } else {
        error = echo_binding(fd, index, local);
    }
    int attempted = 0, waited = 0;
    struct echo_receive receive = {.last_rc = -1};
    if (!error) {
        struct native_echo_message request = {0};
        request.header.icmp6_type = ICMP6_ECHO_REQUEST;
        arc4random_buf(request.nonce, sizeof(request.nonce));
        memcpy(&request.header.icmp6_id, request.nonce, 2);
        memcpy(&request.header.icmp6_seq, request.nonce + 2, 2);
        address.sin6_addr = *peer;
        struct echo_time before = echo_time_at(entry_ns);
        attempted = 1;
        ssize_t n = sendto(fd, &request, sizeof(request), 0,
                           (const struct sockaddr *)&address, sizeof(address));
        int send_error = n < 0 ? errno : 0;
        struct echo_time after = echo_time_at(entry_ns);
        printf("awdl-native: echo_send start_relative_ms=%" PRId64 " start_uptime_ms=%" PRId64
               " return_relative_ms=%" PRId64 " return_uptime_ms=%" PRId64
               " sendto_rc=%zd bytes=%zd errno=%d\n",
               before.relative_ms, before.uptime_ms, after.relative_ms, after.uptime_ms,
               n, n < 0 ? (ssize_t)0 : n, send_error);
        fflush(stdout);
        if (send_error) error = send_error;
        else if (n != sizeof(request)) error = EIO;
        else {
            waited = 1;
            error = echo_wait(fd, index, local, peer, &request, deadline, &receive);
        }
    }
    struct echo_time finished = echo_time_at(entry_ns);
    printf("awdl-native: echo_result send_attempted=%d waited=%d recv_calls=%" PRIu64
           " recv_datagrams=%" PRIu64 " last_recv_rc=%zd last_recv_errno=%d nonce_verified=%d"
           " result_errno=%d relative_ms=%" PRId64 " uptime_ms=%" PRId64 "\n",
           attempted, waited, receive.calls, receive.datagrams, receive.last_rc, receive.last_errno,
           waited && !error, error, finished.relative_ms, finished.uptime_ms);
    fflush(stdout);
    close(fd);
    return error;
}

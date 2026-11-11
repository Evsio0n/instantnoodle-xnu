/* Two typed upper-channel mailboxes. Only the device poller touches a worker
 * or a transport lease. Kernel clients submit/cancel/consume value records
 * under a short mutex. No transfer or IOKit call runs under that mutex. */
#include <sys/hk_abp_nas_worker.h>
#include <sys/hk_qmux_frontend.h>
#ifdef KERNEL
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/clock.h>
static LCK_GRP_DECLARE(front_group, "cellular QMUX front");
static LCK_MTX_DECLARE(front_lock, &front_group);
#define LOCK() lck_mtx_lock(&front_lock)
#define UNLOCK() lck_mtx_unlock(&front_lock)
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
static pthread_mutex_t front_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&front_lock)
#define UNLOCK() pthread_mutex_unlock(&front_lock)
#endif

enum state { FRONT_IDLE, FRONT_QUEUED, FRONT_SENT, FRONT_COMPLETE, FRONT_CANCELLED };
struct offered {
    struct hk_qmux_channel_info info;
    struct hk_qmux_nas_answer answer;
    struct hk_qmux_nas_event event;
    enum state state;
    uint64_t token, began;
    struct hk_qmux_nas_subscription subscription;
    uint64_t subscription_token, subscription_began;
    bool register_sent;
};
struct owner_channel {
    uint64_t lease, published;
    uint8_t cid;
    uint16_t transaction;
    unsigned index;
    bool retiring;
};
static struct {
    /* All public fields below are protected by front_lock. */
    uint64_t generation, next_token;
    struct offered slot[2];
    struct {
        uint64_t generation, token;
        int error;
    } retired[2];
    struct {
        uint64_t generation, token;
        struct hk_qmux_nas_subscription status;
    } last_subscription[2];
} public_state;
static struct {
    /* All fields below are confined to one physical modem poller. */
    struct hk_abp_nas_worker *worker;
    struct owner_channel slot[2];
    uint32_t node, peer;
    bool established;
} owner_state;

static uint64_t now_ns(void)
{
#ifdef HK_QMUX_FRONTEND_TEST
    extern uint64_t hk_qmux_frontend_test_time(void);
    return hk_qmux_frontend_test_time();
#elif defined(KERNEL)
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* Under front_lock. A cancelled or expired request cannot complete into a
 * replacement token. Keep one terminal reason independently of rebind. */
static void retire(unsigned i, int error)
{
    struct offered *s = &public_state.slot[i];
    if (!s->token)
        return;
    if (public_state.retired[i].token == s->token &&
        public_state.retired[i].generation == s->info.generation)
        return;
    public_state.retired[i].generation = s->info.generation;
    public_state.retired[i].token = s->token;
    public_state.retired[i].error = error;
}
static bool registering(const struct offered *s)
{
    return s->subscription.phase == HK_QMUX_SUB_STARTING || s->subscription.phase == HK_QMUX_SUB_STOPPING;
}

static void end_subscription(struct offered *s, int error)
{
    s->subscription.phase = HK_QMUX_SUB_CLOSED;
    s->subscription.terminal_error = error;
}

static void remember_subscription(unsigned i)
{
    struct offered *s = &public_state.slot[i];
    if (!s->subscription_token)
        return;
    if (s->subscription.phase != HK_QMUX_SUB_CLOSED)
        end_subscription(s, ESTALE);
    public_state.last_subscription[i].generation = s->info.generation;
    public_state.last_subscription[i].token = s->subscription_token;
    public_state.last_subscription[i].status = s->subscription;
}

static void expire(unsigned i)
{
    struct offered *s = &public_state.slot[i];
    if (registering(s) && now_ns() - s->subscription_began >= HK_QMUX_FRONTEND_TIMEOUT_NS)
        end_subscription(s, ETIMEDOUT);
    if ((s->state == FRONT_QUEUED || s->state == FRONT_SENT) &&
        now_ns() - s->began >= HK_QMUX_FRONTEND_TIMEOUT_NS) {
        retire(i, ETIMEDOUT);
        if (s->state == FRONT_SENT)
            s->state = FRONT_CANCELLED;
        else {
            s->state = FRONT_IDLE;
            s->token = 0;
        }
    }
}
static unsigned u16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static int admitted(unsigned index, uint64_t generation)
{
    if (index < 1 || index > 2 || !generation)
        return EINVAL;
    if (public_state.slot[index - 1].info.generation != generation)
        return ESTALE;
    return 0;
}

int hk_qmux_frontend_info(unsigned index, struct hk_qmux_channel_info *out)
{
    if (index < 1 || index > 2 || !out)
        return EINVAL;
    LOCK();
    int err = ENOTCONN;
    *out = (struct hk_qmux_channel_info){0};
    if (public_state.slot[index - 1].info.generation) {
        *out = public_state.slot[index - 1].info;
        err = 0;
    }
    UNLOCK();
    return err;
}

static int read_event(unsigned index, uint64_t generation, uint64_t token, uint64_t after_sequence,
                      struct hk_qmux_nas_event *out)
{
    if (!out)
        return EINVAL;
    *out = (struct hk_qmux_nas_event){0};
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        struct offered *s = &public_state.slot[index - 1];
        const struct hk_qmux_nas_event *event = &s->event;
        if (token && s->subscription_token != token)
            err = ENOENT;
        else if (token && s->subscription.phase != HK_QMUX_SUB_ACTIVE)
            err = s->subscription.phase == HK_QMUX_SUB_STARTING ? EAGAIN : ENOTCONN;
        else if (event->sequence == UINT64_MAX)
            err = EOVERFLOW;
        else if (event->sequence <= after_sequence || (token && event->sequence <= s->subscription.sequence_at_ack))
            err = EAGAIN;
        else
            *out = *event;
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_read_event(unsigned index, uint64_t generation, uint64_t after_sequence,
                                struct hk_qmux_nas_event *out)
{
    return read_event(index, generation, 0, after_sequence, out);
}

int hk_qmux_frontend_read_subscription_event(unsigned index, uint64_t generation, uint64_t token,
                                             uint64_t after_sequence, struct hk_qmux_nas_event *out)
{
    if (!token) {
        if (out) *out = (struct hk_qmux_nas_event){0};
        return EINVAL;
    }
    return read_event(index, generation, token, after_sequence, out);
}

int hk_qmux_frontend_subscribe(unsigned index, uint64_t generation, uint64_t *token)
{
    if (!token) return EINVAL;
    *token = 0;
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        struct offered *s = &public_state.slot[index - 1];
        if (s->subscription_token) err = EALREADY;
        else if (s->state != FRONT_IDLE) err = EBUSY;
        else if (public_state.next_token == UINT64_MAX) err = EOVERFLOW;
        else {
            *token = s->subscription_token = ++public_state.next_token;
            s->subscription.phase = HK_QMUX_SUB_STARTING;
            s->subscription_began = now_ns();
        }
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_subscription(unsigned index, uint64_t generation, uint64_t token,
                                  struct hk_qmux_nas_subscription *out)
{
    if (!out) return EINVAL;
    *out = (struct hk_qmux_nas_subscription){0};
    if (!token || !generation || index < 1 || index > 2) return EINVAL;
    unsigned i = index - 1;
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        expire(i);
        if (public_state.slot[i].subscription_token != token) err = ENOENT;
        else *out = public_state.slot[i].subscription;
    } else if (public_state.last_subscription[i].token == token &&
               public_state.last_subscription[i].generation == generation) {
        *out = public_state.last_subscription[i].status;
        err = 0;
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_unsubscribe(unsigned index, uint64_t generation, uint64_t token)
{
    if (!token) return EINVAL;
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        unsigned i = index - 1;
        struct offered *s = &public_state.slot[i];
        expire(i);
        if (s->subscription_token != token) err = ENOENT;
        else if (s->subscription.phase == HK_QMUX_SUB_STOPPING || s->subscription.phase == HK_QMUX_SUB_CLOSED)
            err = EALREADY;
        else if (s->subscription.phase == HK_QMUX_SUB_ACTIVE && s->state == FRONT_COMPLETE)
            err = EBUSY; /* The owner must take or cancel this completed GET before releasing its CID. */
        else if (s->subscription.phase == HK_QMUX_SUB_ACTIVE && s->state == FRONT_IDLE) {
            s->subscription.phase = HK_QMUX_SUB_STOPPING;
            s->subscription_began = now_ns();
            s->register_sent = false;
        } else {
            /* An in-flight exchange has exclusive use of this CID. Closing
             * the actual port is the bounded fallback when a clear cannot run. */
            end_subscription(s, ECANCELED);
        }
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_request(unsigned index, uint64_t generation, uint64_t *token)
{
    if (!token)
        return EINVAL;
    *token = 0;
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        struct offered *s = &public_state.slot[index - 1];
        if (s->state != FRONT_IDLE || registering(s) || s->subscription.phase == HK_QMUX_SUB_CLOSED)
            err = EBUSY;
        else if (public_state.next_token == UINT64_MAX)
            err = EOVERFLOW;
        else {
            s->token = ++public_state.next_token;
            s->began = now_ns();
            s->state = FRONT_QUEUED;
            *token = s->token;
        }
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_take(unsigned index, uint64_t generation, uint64_t token,
                          struct hk_qmux_nas_answer *out)
{
    if (!out)
        return EINVAL;
    *out = (struct hk_qmux_nas_answer){0};
    if (!token || index < 1 || index > 2 || !generation)
        return EINVAL;
    LOCK();
    expire(index - 1);
    if (public_state.retired[index - 1].token == token &&
        public_state.retired[index - 1].generation == generation) {
        int terminal = public_state.retired[index - 1].error;
        UNLOCK();
        return terminal;
    }
    int err = admitted(index, generation);
    if (!err) {
        struct offered *s = &public_state.slot[index - 1];
        if (s->token != token || s->state == FRONT_IDLE)
            err = ENOENT;
        else if (s->state == FRONT_CANCELLED)
            err = ECANCELED;
        else if (s->state != FRONT_COMPLETE)
            err = EAGAIN;
        else {
            *out = s->answer;
            s->state = FRONT_IDLE;
            s->token = 0;
        }
    }
    UNLOCK();
    return err;
}

int hk_qmux_frontend_cancel(unsigned index, uint64_t generation, uint64_t token)
{
    if (!token)
        return EINVAL;
    LOCK();
    int err = admitted(index, generation);
    if (!err) {
        struct offered *s = &public_state.slot[index - 1];
        if (s->token != token || s->state == FRONT_IDLE)
            err = ENOENT;
        else {
            retire(index - 1, ECANCELED);
            if (s->state == FRONT_SENT || s->state == FRONT_CANCELLED)
                s->state = FRONT_CANCELLED;
            else {
                s->state = FRONT_IDLE;
                s->token = 0;
            }
        }
    }
    UNLOCK();
    return err;
}

static void register_reply(struct owner_channel *c, const uint8_t *p, size_t n)
{
    if (p[0] != 2 || u16(p + 1) != c->transaction) return;
    const uint8_t *result = hk_qmux_nas_result_tlv(p + 7, n - 7);
    if (!result) return;
    struct hk_qmux_register_ack ack = {
        .transaction = c->transaction, .response_bytes = (uint16_t)n,
        .firmware_result = (uint16_t)u16(result), .firmware_error = (uint16_t)u16(result + 2)};
    bool success = !ack.firmware_result && !ack.firmware_error;
    LOCK();
    unsigned i = c->index - 1;
    struct offered *s = &public_state.slot[i];
    expire(i);
    if (s->info.generation == c->published && s->register_sent) {
        if (s->subscription.phase == HK_QMUX_SUB_STARTING) {
            s->subscription.enable = ack;
            s->subscription.sequence_at_ack = s->event.sequence;
            s->subscription.phase = success ? HK_QMUX_SUB_ACTIVE : HK_QMUX_SUB_REFUSED;
        } else if (s->subscription.phase == HK_QMUX_SUB_STOPPING) {
            s->subscription.disable = ack;
            end_subscription(s, success ? 0 : EIO);
        }
    }
    UNLOCK();
}

static void emit(void *context, const uint8_t h[6], const uint8_t *p, size_t n)
{
    struct owner_channel *c = context;
    if (h[4] == 0 && n == 18 && p[2] == 0x22 && p[17]) {
        c->cid = p[17];
        return;
    }
    if (h[4] != 3 || h[5] != c->cid || !c->published || n < 7 ||
        u16(p + 5) != n - 7)
        return;
    if (u16(p + 3) == 3) {
        register_reply(c, p, n);
        return;
    }
    if (u16(p + 3) != 0x24) return;
    bool indication = p[0] == 4;
    if (!indication && (p[0] != 2 || u16(p + 1) != c->transaction))
        return;
    struct hk_qmux_nas_answer a = {.transaction = c->transaction, .response_bytes = (uint16_t)n};
    bool result = false, saw_serving = false, serving = false;
    for (size_t off = 7; off < n;) {
        if (n - off < 3)
            return;
        unsigned type = p[off], len = u16(p + off + 1);
        off += 3;
        if (len > n - off)
            return;
        if (type == 2) {
            if (indication || result || len != 4)
                return;
            a.firmware_result = (uint16_t)u16(p + off);
            a.firmware_error = (uint16_t)u16(p + off + 2);
            result = true;
        } else if (type == 1) {
            if (saw_serving)
                return;
            saw_serving = true;
            /* libqmi NAS 0x24 TLV 1: registration 0..4, attach 0..2,
             * selected network, radio count and exactly that many bytes.
             * The typed API does not expose selected network or radio IDs. */
            if (len >= 5 && (unsigned)p[off + 4] == len - 5 &&
                p[off] <= 4 && p[off + 1] <= 2 && p[off + 2] <= 2) {
                a.registration = p[off];
                a.cs_attach = p[off + 1];
                a.ps_attach = p[off + 2];
                serving = true;
            }
        }
        off += len;
    }
    if (indication) {
        if (!serving)
            return;
        /* The router checked peer + local port before applying the CID.
         * This mailbox is independent of the response token and deadline. */
        LOCK();
        struct offered *s = &public_state.slot[c->index - 1];
        if (s->info.generation == c->published && s->event.sequence < UINT64_MAX) {
            s->event = (struct hk_qmux_nas_event){
                .channel = s->info, .sequence = s->event.sequence + 1, .received_ns = now_ns(),
                .registration = a.registration, .cs_attach = a.cs_attach, .ps_attach = a.ps_attach};
        }
        UNLOCK();
        return;
    }
    if (!result)
        return;
    a.serving_valid = !a.firmware_result && !a.firmware_error && serving;
    if (!a.serving_valid) {
        a.registration = 0;
        a.cs_attach = 0;
        a.ps_attach = 0;
    }
    LOCK();
    struct offered *s = &public_state.slot[c->index - 1];
    expire(c->index - 1);
    if (s->info.generation == c->published && s->state == FRONT_SENT) {
        s->answer = a;
        s->state = FRONT_COMPLETE;
    }
    UNLOCK();
}

static bool owner_thread(const struct hk_abp_nas_worker *w)
{
    return w && w->ops && w->ops->is_owner(w->context);
}

static bool close_slot(struct hk_abp_nas_worker *w, unsigned i)
{
    struct owner_channel *c = &owner_state.slot[i];
    c->retiring = true;
    LOCK();
    retire(i, ESTALE);
    remember_subscription(i);
    public_state.slot[i] = (struct offered){0};
    UNLOCK();
    if (c->published && !hk_qmux_registry_withdraw(i + 1, c->published))
        return false;
    if (c->lease && hk_qo_phase(&w->gate) == HK_QO_READY) {
        int error = hk_abp_worker_close(w, c->lease);
        if (error && error != ENOENT && error != ECANCELED)
            return false;
    }
    *c = (struct owner_channel){.index = i + 1};
    return true;
}

static bool withdraw(struct hk_abp_nas_worker *w)
{
    /* Stop admission for both channels before retiring either real port. */
    LOCK();
    for (unsigned i = 0; i < 2; i++) {
        retire(i, ESTALE);
        remember_subscription(i);
        public_state.slot[i] = (struct offered){0};
    }
    UNLOCK();
    bool a = close_slot(w, 0);
    bool b = close_slot(w, 1);
    if (!a || !b)
        return false;
    owner_state.node = 0;
    owner_state.peer = 0;
    owner_state.established = false;
    return true;
}

bool hk_qmux_frontend_stop(struct hk_abp_nas_worker *w)
{
    if (!owner_thread(w))
        return false;
    if (!owner_state.worker)
        return true;
    if (owner_state.worker != w || !withdraw(w))
        return false;
    /* If STOP already closed worker admission, its final detach reclaims its
     * own leases. No front object or queued result points into that worker. */
    owner_state.worker = NULL;
    return true;
}

static int open_slot(struct hk_abp_nas_worker *w, unsigned i)
{
    static const char *const names[] = {"qmux1", "qmux2"};
    struct owner_channel *c = &owner_state.slot[i];
    c->index = i + 1;
    int err = hk_abp_worker_open(w, names[i], 5, emit, c, &c->lease);
    if (err)
        return err;
    const uint8_t alloc[] = {1, 15, 0, 0, 0, 0, 0, 1, 0x22, 0, 4, 0, 1, 1, 0, 3};
    err = hk_abp_worker_write(w, c->lease, alloc, sizeof(alloc));
    if (!err && !c->cid)
        err = EPROTO;
    return err;
}

static bool current(struct hk_abp_nas_worker *w, unsigned i, struct hk_qmux_channel_info *info)
{
    struct owner_channel *c = &owner_state.slot[i];
    struct hk_abp_nas_channel *binding = &w->backend.channels[i];
    if (!c->lease || c->lease != binding->lease || binding->revoked || !c->cid ||
        c->cid > HK_QMUX_NAS_CLIENTS)
        return false;
    const struct hk_qmux_nas_slot *s = &binding->router.slots[c->cid - 1];
    uint32_t local = hk_qmux_nas_local_port(&binding->router, c->cid);
    if (!local || !s->ready || s->lost || s->node != w->backend.node || s->server != w->backend.port)
        return false;
    *info = (struct hk_qmux_channel_info){
        .index = i + 1, .node = s->node, .remote_port = s->server, .local_port = local, .cid = c->cid};
    return true;
}

void hk_qmux_frontend_poll(struct hk_abp_nas_worker *w)
{
    if (!owner_thread(w) || !w->attached || hk_qo_phase(&w->gate) != HK_QO_READY)
        return;
    if (owner_state.worker && owner_state.worker != w)
        return;
    owner_state.worker = w;
    if (!w->backend.node || !w->backend.port) {
        (void)withdraw(w);
        return;
    }
    if (owner_state.node &&
        (owner_state.node != w->backend.node || owner_state.peer != w->backend.port)) {
        (void)withdraw(w);
        return;
    }
    struct hk_qmux_channel_info views[2];
    bool ready[2] = {false, false};
    for (unsigned i = 0; i < 2; i++) {
        struct owner_channel *c = &owner_state.slot[i];
        if (w->backend.channels[i].revoked) {
            (void)withdraw(w);
            return;
        }
        if (c->retiring && !close_slot(w, i))
            continue;
        if (!c->lease && open_slot(w, i)) {
            (void)close_slot(w, i);
            continue;
        }
        ready[i] = current(w, i, &views[i]);
        if (!ready[i] && c->published)
            (void)close_slot(w, i);
    }
    if (!owner_state.established && (!ready[0] || !ready[1]))
        return;
    owner_state.node = w->backend.node;
    owner_state.peer = w->backend.port;
    for (unsigned i = 0; i < 2; i++) {
        struct owner_channel *c = &owner_state.slot[i];
        if (c->published || !ready[i])
            continue;
        LOCK();
        if (public_state.generation == UINT64_MAX) {
            UNLOCK();
            (void)withdraw(w);
            return;
        }
        views[i].generation = ++public_state.generation;
        UNLOCK();
        if (!hk_qmux_registry_publish(&views[i])) {
            if (!owner_state.established) {
                (void)withdraw(w);
                return;
            }
            (void)close_slot(w, i);
            continue;
        }
        c->published = views[i].generation;
        LOCK();
        public_state.slot[i].info = views[i];
        UNLOCK();
    }
    if (owner_state.slot[0].published && owner_state.slot[1].published)
        owner_state.established = true;
    for (unsigned i = 0; i < 2; i++) {
        struct owner_channel *c = &owner_state.slot[i];
        if (!c->published || c->retiring)
            continue;
        LOCK();
        struct offered *s = &public_state.slot[i];
        expire(i);
        enum state state = s->state;
        enum hk_qmux_subscription_phase phase = s->subscription.phase;
        bool send_register = registering(s) && !s->register_sent;
        if (send_register) s->register_sent = true;
        if (state == FRONT_QUEUED)
            s->state = FRONT_SENT;
        UNLOCK();
        if (state == FRONT_CANCELLED || phase == HK_QMUX_SUB_CLOSED) {
            /* This transport supports one pending request per CID. Rebind
             * before accepting a retry, even when the transaction wraps. The
             * registry advertises exact local ports, so replace this channel only. */
            (void)close_slot(w, i);
            continue;
        }
        if (state != FRONT_QUEUED && !send_register)
            continue;
        if (c->transaction == UINT16_MAX) {
            (void)close_slot(w, i);
            continue;
        }
        c->transaction++;
        uint8_t request[] = {
            1,    12, 0, 0, 3, c->cid, 0, (uint8_t)c->transaction, (uint8_t)(c->transaction >> 8),
            0x24, 0, 0, 0, 0x13, 1, 0, 0};
        size_t length = 13;
        if (send_register) {
            request[1] = 16;
            request[9] = 3;
            request[11] = 4;
            request[16] = phase == HK_QMUX_SUB_STARTING;
            length = sizeof(request);
        }
        int write_error = hk_abp_worker_write(w, c->lease, request, length);
        if (write_error) {
            if (send_register) {
                LOCK();
                if (registering(s)) end_subscription(s, write_error);
                UNLOCK();
            }
            (void)close_slot(w, i);
        }
    }
}

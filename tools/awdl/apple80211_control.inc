/* Included by hk_apple80211.c in the opt-in overlay. The dedicated mutex
 * guards state only. It is not held over ifnet lookup, user memory or calls
 * to the device owner. Retiring a provider cannot invalidate a live callback. */

struct hk_awdl_create_request {
    uint32_t version;
    uint8_t address[6];
    uint8_t reserved[2];
    uint32_t role;
    char name[HK_AWDL_NAME_SIZE];
};
struct hk_awdl_delete_request {
    uint32_t version;
    char name[HK_AWDL_NAME_SIZE];
};
struct hk_awdl_channel_reply {
    uint32_t version;
    struct hk_awdl_channels channels;
    uint8_t reserved[2];
};
_Static_assert(sizeof(struct hk_awdl_create_request) == 32, "20F66 create");
_Static_assert(offsetof(struct hk_awdl_create_request, role) == 12, "20F66 role");
_Static_assert(offsetof(struct hk_awdl_create_request, name) == 16, "20F66 returned name");
_Static_assert(sizeof(struct hk_awdl_delete_request) == 20, "20F66 delete");
_Static_assert(sizeof(struct hk_awdl_channel_reply) == 12, "20F66 channel reply");

static struct hk_awdl_control_state {
    struct hk_awdl_ops ops;
    void *context;
    uint64_t generation;
    uint64_t lease;
    uint64_t diagnostic_owner;
    char name[HK_AWDL_NAME_SIZE];
    bool ready;
    bool retiring;
    bool busy;
    bool cleanup;
} awdl;
static uint64_t awdl_sequence;
#ifdef HK_AWDL_NATIVE_ADDRESS
#include "native_address.inc"
#endif

static uint64_t
awdl_next_locked(void)
{
    /* Refuse new ownership on wrap instead of reusing a stale token. */
    if (awdl_sequence == UINT64_MAX)
        return 0;
    return ++awdl_sequence;
}

static bool
awdl_valid_name(const char name[HK_AWDL_NAME_SIZE])
{
    return name[0] && memchr(name, '\0', HK_AWDL_NAME_SIZE) != NULL;
}

int
hk_awdl_provider_register(const struct hk_awdl_ops *ops, void *context, uint64_t *generation)
{
    if (!generation || !ops || !ops->create || !ops->remove || !ops->channels)
        return EINVAL;
    HK_AWDL_LOCK();
    int error = 0;
    if (awdl.generation) {
        error = EBUSY;
    } else if (!(awdl.generation = awdl_next_locked())) {
        error = EOVERFLOW;
    } else {
        awdl.ops = *ops;
        awdl.context = context;
        *generation = awdl.generation;
    }
    HK_AWDL_UNLOCK();
    return error;
}

int
hk_awdl_provider_set_ready(uint64_t generation, bool ready)
{
    HK_AWDL_LOCK();
    int error = 0;
    if (!generation || generation != awdl.generation)
        error = ENOENT;
    else if (awdl.retiring)
        error = EBUSY;
    else if (ready && awdl.cleanup)
        error = EBUSY;
    else {
        awdl.ready = ready;
#ifdef HK_AWDL_NATIVE_ADDRESS
        wakeup(&awdl.ready);
#endif
        if (!ready && awdl.lease)
            awdl.cleanup = true;
    }
    HK_AWDL_UNLOCK();
    return error;
}

static bool
awdl_capable(const char *parent)
{
    HK_AWDL_LOCK();
    bool ready = !strncmp(parent, "en0", HK_AWDL_NAME_SIZE) &&
                 awdl.generation && awdl.ready && !awdl.retiring && !awdl.cleanup;
#ifdef HK_AWDL_NATIVE_ADDRESS
    /* A worker waiting for CREATE has hardware available to take a request.
     * Data readiness still requires the real firmware grant. */
    ready |= awdl_address.waiting && !strncmp(parent,"en0",HK_AWDL_NAME_SIZE);
#endif
    HK_AWDL_UNLOCK();
    return ready;
}

static void
awdl_forget_locked(void)
{
    awdl.lease = 0;
    awdl.diagnostic_owner = 0;
    memset(awdl.name, 0, sizeof(awdl.name));
    awdl.cleanup = false;
}

/* Enter with busy owned and without the lock. A nonzero remove result
 * preserves the lease even if the caller never received an interface name. */
static int
awdl_remove_lease(void)
{
    int error = awdl.ops.remove(awdl.context, awdl.lease);
    HK_AWDL_LOCK();
    if (!error)
        awdl_forget_locked();
    else
        awdl.cleanup = true;
    HK_AWDL_UNLOCK();
    return error;
}

int
hk_awdl_provider_unregister(uint64_t generation)
{
    HK_AWDL_LOCK();
    if (!generation || generation != awdl.generation) {
        HK_AWDL_UNLOCK();
        return ENOENT;
    }
    awdl.retiring = true;
    awdl.ready = false;
    if (awdl.busy) {
        HK_AWDL_UNLOCK();
        return EBUSY;
    }
    awdl.busy = true;
    bool leased = awdl.lease != 0;
    HK_AWDL_UNLOCK();

    int error = leased ? awdl_remove_lease() : 0;
    HK_AWDL_LOCK();
    if (!error)
        memset(&awdl, 0, sizeof(awdl));
    else
        awdl.busy = false;
    HK_AWDL_UNLOCK();
    return error;
}

static int
awdl_create(const struct apple80211req *request)
{
    struct hk_awdl_create_request wire;
    const uint8_t zero[6] = {0};
    if (strncmp(request->req_if_name, "en0", HK_AWDL_NAME_SIZE) || request->req_len != sizeof(wire))
        return EINVAL;
    int error = copyin(request->req_data, &wire, sizeof(wire));
    if (error)
        return error;
    if (wire.version != 1)
        return EINVAL;
    if (wire.role != 6 || (wire.address[0] & 1) || wire.reserved[0] || wire.reserved[1] || wire.name[0])
        return EOPNOTSUPP;

    HK_AWDL_LOCK();
#ifdef HK_AWDL_NATIVE_ADDRESS
    if (!awdl.ready && !awdl.retiring && memcmp(wire.address, zero, sizeof(zero))) {
        error = awdl_supply_address_locked(wire.address);
        if (error) {HK_AWDL_UNLOCK();return error;}
    }
#endif
    if (!awdl.generation || !awdl.ready || awdl.retiring) {
        HK_AWDL_UNLOCK();
        return EOPNOTSUPP;
    }
    if (awdl.busy || awdl.cleanup) {
        HK_AWDL_UNLOCK();
        return EBUSY;
    }
    bool fresh = awdl.lease == 0;
    bool diagnostic = request->req_val == HK_AWDL_DIAG_GUARD;
    uint64_t client = hk_awdl_client_id();
    if (diagnostic && (!fresh || !client)) {
        HK_AWDL_UNLOCK();
        return fresh ? EINVAL : EEXIST;
    }
    if (fresh && !(awdl.lease = awdl_next_locked())) {
        HK_AWDL_UNLOCK();
        return EOVERFLOW;
    }
    awdl.busy = true;
    char name[HK_AWDL_NAME_SIZE] = {0};
    if (!fresh)
        memcpy(name, awdl.name, sizeof(name));
    HK_AWDL_UNLOCK();

    if (fresh)
        error = awdl.ops.create(awdl.context, awdl.lease, name);
    if (!error && (!awdl_valid_name(name) || !strcmp(name, "en0")))
        error = EIO;
    if (!error && !hk_awdl_name_attached(name))
        error = ENXIO;

    HK_AWDL_LOCK();
    if (error && !fresh)
        awdl.cleanup = true;
    if (!error && (!awdl.ready || awdl.retiring || awdl.cleanup))
        error = ENXIO;
    if (!error && fresh)
        memcpy(awdl.name, name, sizeof(name));
    HK_AWDL_UNLOCK();

    if (!error && memcmp(wire.address, zero, sizeof(zero)) &&
        !hk_awdl_address_matches(name,wire.address))
        error = EADDRNOTAVAIL;
    if (!error) {
        memset(wire.name, 0, sizeof(wire.name));
        memcpy(wire.name, name, strlen(name));
        error = copyout(&wire, request->req_data, sizeof(wire));
    }
    HK_AWDL_LOCK();
    if (!error && (!awdl.ready || awdl.retiring || awdl.cleanup))
        error = ENXIO;
    if (!error || !fresh) {
        if (!error && fresh && diagnostic)
            awdl.diagnostic_owner = client;
        else if (!error && !fresh && awdl.diagnostic_owner != client)
            awdl.diagnostic_owner = 0; /* ordinary consumer acquired it */
        awdl.busy = false;
        HK_AWDL_UNLOCK();
        return error;
    }
    HK_AWDL_UNLOCK();
    /* The original operation's errno is returned to user space. Keep a
     * failed rollback pinned; unregister retries its exact lease. */
    (void)awdl_remove_lease();
    HK_AWDL_LOCK();
    awdl.busy = false;
    HK_AWDL_UNLOCK();
    return error;
}

static int
awdl_delete(const struct apple80211req *request)
{
    struct hk_awdl_delete_request wire;
    if (strncmp(request->req_if_name, "en0", HK_AWDL_NAME_SIZE) || request->req_len != sizeof(wire))
        return EINVAL;
    int error = copyin(request->req_data, &wire, sizeof(wire));
    if (error)
        return error;
    if (wire.version != 1 || !awdl_valid_name(wire.name))
        return EINVAL;
    HK_AWDL_LOCK();
    if (!awdl.generation) error = EOPNOTSUPP;
    else if (awdl.busy) error = EBUSY;
    else if (!awdl.lease || strcmp(awdl.name, wire.name)) error = ENOENT;
    else if (request->req_val == HK_AWDL_DIAG_GUARD &&
             (!awdl.diagnostic_owner || awdl.diagnostic_owner != hk_awdl_client_id())) error = EBUSY;
    if (!error) awdl.busy = true;
    HK_AWDL_UNLOCK();
    if (error) return error;
    error = awdl_remove_lease();
    HK_AWDL_LOCK();
    awdl.busy = false;
    HK_AWDL_UNLOCK();
    return error;
}

static int
awdl_channels(const struct apple80211req *request)
{
    struct hk_awdl_channel_reply wire = {.version = 1};
    if (request->req_len != sizeof(wire) || !awdl_valid_name(request->req_if_name))
        return EINVAL;
    HK_AWDL_LOCK();
    int error = 0;
    if (!awdl.generation || !awdl.ready || awdl.retiring || awdl.cleanup) error = EOPNOTSUPP;
    else if (awdl.busy) error = EBUSY;
    else if (!awdl.lease) error = ENODATA;
    else if (strcmp(request->req_if_name, "en0") && strcmp(request->req_if_name, awdl.name)) error = ENXIO;
    if (!error) awdl.busy = true;
    HK_AWDL_UNLOCK();
    if (error) return error;

    error = awdl.ops.channels(awdl.context, awdl.lease, &wire.channels);
    HK_AWDL_LOCK();
    if (!error && (!awdl.ready || awdl.retiring || awdl.cleanup))
        error = ENXIO;
    HK_AWDL_UNLOCK();
    if (!error)
        error = copyout(&wire, request->req_data, sizeof(wire));
    HK_AWDL_LOCK();
    if (!error && (!awdl.ready || awdl.retiring || awdl.cleanup))
        error = ENXIO;
    awdl.busy = false;
    HK_AWDL_UNLOCK();
    return error;
}

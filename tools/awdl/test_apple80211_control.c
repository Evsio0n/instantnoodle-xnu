/* Host fixture for the actual Apple80211 control include. No interface or
 * firmware operations occur. The provider publishes only fixture-owned data. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apple80211_provider.h"

struct apple80211req {
    char req_if_name[16];
    int32_t req_type, req_val;
    uint32_t req_len;
    uintptr_t req_data __attribute__((aligned(8)));
};

static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool holding_control;
static void lock_control(void)
{
    assert(!holding_control);
    assert(pthread_mutex_lock(&control_mutex) == 0);
    holding_control = true;
}
static void unlock_control(void)
{
    assert(holding_control);
    holding_control = false;
    assert(pthread_mutex_unlock(&control_mutex) == 0);
}
#define HK_AWDL_LOCK() lock_control()
#define HK_AWDL_UNLOCK() unlock_control()

static bool fail_copyin, fail_copyout;
static void fixture_copyout_gate(void);
static int copyin(uintptr_t from, void *to, size_t n)
{
    assert(!holding_control);
    if (!from || fail_copyin)
        return EFAULT;
    memcpy(to, (const void *)from, n);
    return 0;
}
static int copyout(const void *from, uintptr_t to, size_t n)
{
    assert(!holding_control);
    if (!to || fail_copyout)
        return EFAULT;
    memcpy((void *)to, from, n);
    fixture_copyout_gate();
    return 0;
}

enum callback { NONE, CREATE, CHANNELS, COPYOUT };
struct fixture {
    unsigned created, removed, queried;
    uint64_t active_lease, last_remove;
    int remove_error, create_error, channels_error;
    char name[16];
    bool attached, publish;
    struct hk_awdl_channels status;
    enum callback blocked;
    bool entered, resume;
};

static struct fixture f;
static pthread_mutex_t fixture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static void enter_fixture(enum callback callback)
{
    assert(!holding_control);
    assert(pthread_mutex_lock(&fixture_mutex) == 0);
    if (callback != NONE && f.blocked == callback) {
        f.entered = true;
        assert(pthread_cond_broadcast(&gate) == 0);
        while (!f.resume)
            assert(pthread_cond_wait(&gate, &fixture_mutex) == 0);
    }
}
static void leave_fixture(void)
{
    assert(pthread_mutex_unlock(&fixture_mutex) == 0);
}
static void fixture_copyout_gate(void)
{
    enter_fixture(COPYOUT);
    leave_fixture();
}
static uint64_t test_client_id = 1;
static uint64_t hk_awdl_client_id(void) { return test_client_id; }
static bool hk_awdl_name_attached(const char *name)
{
    enter_fixture(NONE);
    bool attached = f.attached && !strcmp(name, f.name);
    leave_fixture();
    return attached;
}
static bool hk_awdl_address_matches(const char *name,const uint8_t address[6])
{
    static const uint8_t actual[6]={2,0,0,0,0,1};
    return hk_awdl_name_attached(name) && !memcmp(actual,address,6);
}
#include "apple80211_control.inc"

static int create_interface(void *context, uint64_t lease, char name[16])
{
    assert(context == &f && lease);
    enter_fixture(CREATE);
    assert(!f.active_lease);
    f.created++;
    f.active_lease = lease;
    f.attached = f.publish;
    memcpy(name, f.name, 16);
    int error = f.create_error;
    leave_fixture();
    return error;
}
static int remove_interface(void *context, uint64_t lease)
{
    assert(context == &f);
    enter_fixture(NONE);
    assert(lease && lease == f.active_lease);
    f.last_remove = lease;
    f.removed++;
    int error = f.remove_error;
    if (!error) {
        f.attached = false;
        f.active_lease = 0;
    }
    leave_fixture();
    return error;
}
static int read_channels(void *context, uint64_t lease, struct hk_awdl_channels *channels)
{
    assert(context == &f);
    enter_fixture(CHANNELS);
    assert(lease && lease == f.active_lease);
    f.queried++;
    int error = f.channels_error;
    if (!error)
        *channels = f.status;
    leave_fixture();
    return error;
}
static const struct hk_awdl_ops ops = {create_interface, remove_interface, read_channels};
static struct apple80211req make_request(const char *name, void *bytes, size_t n)
{
    struct apple80211req req = {.req_len = (uint32_t)n, .req_data = (uintptr_t)bytes};
    assert(strlen(name) < sizeof(req.req_if_name));
    strcpy(req.req_if_name, name);
    return req;
}
static void reset_fixture(void)
{
    assert(!awdl.generation && !awdl.lease);
    memset(&f, 0, sizeof(f));
    strcpy(f.name, "awdl42");
    f.publish = true;
    fail_copyin = false;
    fail_copyout = false;
}
static uint64_t register_provider(bool ready)
{
    uint64_t generation = 0;
    assert(hk_awdl_provider_register(&ops, &f, &generation) == 0 && generation);
    if (ready)
        assert(hk_awdl_provider_set_ready(generation, true) == 0);
    return generation;
}
static void test_control(void)
{
    reset_fixture();
    struct hk_awdl_create_request create = {.version = 1, .role = 6};
    struct hk_awdl_delete_request del = {.version = 1, .name = "awdl42"};
    struct hk_awdl_channel_reply reply = {0};
    struct apple80211req c = make_request("en0", &create, sizeof(create));
    struct apple80211req d = make_request("en0", &del, sizeof(del));
    struct apple80211req g = make_request("en0", &reply, sizeof(reply));
    assert(sizeof(c) == 40);
    assert(!awdl_capable("en0"));
    assert(awdl_create(&c) == EOPNOTSUPP && awdl_delete(&d) == EOPNOTSUPP);
    assert(awdl_channels(&g) == EOPNOTSUPP);
    uint64_t generation = register_provider(false);
    uint64_t another = 0;
    assert(hk_awdl_provider_register(&ops, &f, &another) == EBUSY && !another);
    assert(!awdl_capable("en0") && awdl_create(&c) == EOPNOTSUPP);
    assert(hk_awdl_provider_set_ready(generation, true) == 0);
    assert(awdl_capable("en0") && !awdl_capable("en1"));
    assert(awdl_channels(&g) == ENODATA && f.queried == 0);
    c.req_len--;
    assert(awdl_create(&c) == EINVAL);
    c.req_len++;
    create.version++;
    assert(awdl_create(&c) == EINVAL);
    create.version = 1;
    create.role = 4;
    assert(awdl_create(&c) == EOPNOTSUPP);
    create.role = 6;
    create.address[0] = 1;
    assert(awdl_create(&c) == EOPNOTSUPP);
    create.address[0] = 0;
    fail_copyin = true;
    assert(awdl_create(&c) == EFAULT);
    fail_copyin = false;
    assert(f.created == 0);

    assert(awdl_create(&c) == 0 && !strcmp(create.name, "awdl42"));
    assert(f.created == 1 && f.attached);
    uint64_t lease = f.active_lease;
    memset(create.name,0,sizeof(create.name));
    create.address[0]=2;
    assert(awdl_create(&c)==EADDRNOTAVAIL && f.active_lease==lease);
    create.address[5]=1;
    assert(awdl_create(&c)==0 && !strcmp(create.name,"awdl42"));
    memset(create.address,0,sizeof(create.address));
    memset(create.name, 0, sizeof(create.name));
    assert(awdl_create(&c) == 0 && !strcmp(create.name, "awdl42"));
    assert(f.created == 1 && f.active_lease == lease);
    memset(create.name, 0, sizeof(create.name));
    fail_copyout = true;
    assert(awdl_create(&c) == EFAULT && f.active_lease == lease && f.removed == 0);
    fail_copyout = false;

    f.channels_error = ENODATA;
    memset(&reply, 0x5a, sizeof(reply));
    struct hk_awdl_channel_reply previous = reply;
    assert(awdl_channels(&g) == ENODATA && !memcmp(&reply, &previous, sizeof(reply)));
    f.channels_error = 0;
    f.status = (struct hk_awdl_channels){.preferred = 44};
    assert(awdl_channels(&g) == 0 && reply.channels.preferred == 44 && !reply.channels.peer_master);
    f.status = (struct hk_awdl_channels){.peer_master = 149};
    assert(awdl_channels(&g) == 0 && !reply.channels.preferred && reply.channels.peer_master == 149);
    f.status = (struct hk_awdl_channels){.preferred = 44, .peer_master = 149, .extension = {7, 9}};
    g = make_request(f.name, &reply, sizeof(reply));
    assert(awdl_channels(&g) == 0 && reply.version == 1);
    assert(!memcmp(&reply.channels, &f.status, sizeof(f.status)));
    assert(!reply.reserved[0] && !reply.reserved[1]);
    g = make_request("en9", &reply, sizeof(reply));
    assert(awdl_channels(&g) == ENXIO);
    g.req_len = 0;
    assert(awdl_channels(&g) == EINVAL);
    strcpy(del.name, "awdl0");
    assert(awdl_delete(&d) == ENOENT && f.removed == 0);
    strcpy(del.name, "awdl42");
    assert(awdl_delete(&d) == 0 && !f.attached && f.last_remove == lease);
    assert(awdl_delete(&d) == ENOENT);
    assert(hk_awdl_provider_unregister(generation) == 0);
    uint64_t next = register_provider(true);
    assert(next > lease && hk_awdl_provider_set_ready(generation, true) == ENOENT);
    assert(hk_awdl_provider_unregister(generation) == ENOENT && awdl_capable("en0"));
    assert(hk_awdl_provider_unregister(next) == 0);
}

static void test_rollback(void)
{
    reset_fixture();
    uint64_t generation = register_provider(true);
    struct hk_awdl_create_request create = {.version = 1, .role = 6};
    struct apple80211req c = make_request("en0", &create, sizeof(create));
    f.publish = false;
    assert(awdl_create(&c) == ENXIO && f.removed == 1 && !f.active_lease);
    f.publish = true;
    f.create_error = EIO;
    assert(awdl_create(&c) == EIO && f.removed == 2 && !f.active_lease);
    f.create_error = 0;
    f.remove_error = ETIMEDOUT;
    fail_copyout = true;
    assert(awdl_create(&c) == EFAULT && f.removed == 3 && f.active_lease && f.attached);
    uint64_t pinned = f.active_lease;
    assert(!create.name[0] && !awdl_capable("en0"));
    assert(hk_awdl_provider_set_ready(generation, true) == EBUSY);
    assert(hk_awdl_provider_unregister(generation) == ETIMEDOUT && f.active_lease == pinned);
    assert(hk_awdl_provider_set_ready(generation, true) == EBUSY);
    f.remove_error = 0;
    assert(hk_awdl_provider_unregister(generation) == 0 && !f.active_lease);
    assert(f.last_remove == pinned && !f.attached);
}

struct job {
    enum callback callback;
    struct apple80211req req;
    int result;
};
static void *call_control(void *arg)
{
    struct job *job = arg;
    job->result = job->callback == CREATE ? awdl_create(&job->req) : awdl_channels(&job->req);
    return NULL;
}
static void test_blocked_request(enum callback callback)
{
    reset_fixture();
    uint64_t generation = register_provider(true);
    struct hk_awdl_create_request create = {.version = 1, .role = 6};
    struct hk_awdl_channel_reply reply = {0};
    struct apple80211req c = make_request("en0", &create, sizeof(create));
    struct job job = {.callback = callback == CHANNELS ? CHANNELS : CREATE, .req = c};
    if (callback == CHANNELS) {
        f.status = (struct hk_awdl_channels){.preferred = 44, .peer_master = 149};
        assert(awdl_create(&c) == 0);
        job.req = make_request("en0", &reply, sizeof(reply));
    }
    f.blocked = callback;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, call_control, &job) == 0);
    enter_fixture(NONE);
    while (!f.entered)
        assert(pthread_cond_wait(&gate, &fixture_mutex) == 0);
    leave_fixture();
    if (callback == COPYOUT)
        assert(hk_awdl_provider_set_ready(generation, false) == 0);
    else
        assert(hk_awdl_provider_unregister(generation) == EBUSY);
    assert(!awdl_capable("en0"));
    assert(hk_awdl_provider_set_ready(generation, true) == EBUSY);
    enter_fixture(NONE);
    f.resume = true;
    assert(pthread_cond_broadcast(&gate) == 0);
    leave_fixture();
    assert(pthread_join(thread, NULL) == 0 && job.result == ENXIO);
    if (callback == CREATE)
        assert(!create.name[0]);
    else if (callback == CHANNELS)
        assert(!reply.version);
    else
        assert(f.removed == 1 && !f.attached); /* copyout finished, ioctl failed, rollback complete */
    assert(hk_awdl_provider_unregister(generation) == 0);
    assert(f.removed == 1 && !f.attached && !f.active_lease);
}

static void test_lost_attachment(void)
{
    reset_fixture();
    uint64_t generation = register_provider(true);
    struct hk_awdl_create_request create = {.version = 1, .role = 6};
    struct apple80211req c = make_request("en0", &create, sizeof(create));
    assert(awdl_create(&c) == 0);
    uint64_t lease = f.active_lease;
    f.attached = false;
    memset(create.name, 0, sizeof(create.name));
    assert(awdl_create(&c) == ENXIO && !awdl_capable("en0"));
    assert(hk_awdl_provider_set_ready(generation, true) == EBUSY);
    assert(f.active_lease == lease && f.removed == 0);
    assert(hk_awdl_provider_unregister(generation) == 0);
    assert(f.last_remove == lease && !f.active_lease);
}

static void test_diagnostic_ownership(void)
{
    reset_fixture();
    uint64_t generation=register_provider(true);
    struct hk_awdl_create_request wire={.version=1,.role=6};
    struct hk_awdl_delete_request remove={.version=1,.name="awdl42"};
    struct apple80211req c=make_request("en0",&wire,sizeof(wire));
    struct apple80211req d=make_request("en0",&remove,sizeof(remove));
    test_client_id=11;
    c.req_val=d.req_val=HK_AWDL_DIAG_GUARD;
    assert(awdl_create(&c)==0 && f.created==1);
    wire.name[0]=0;
    assert(awdl_create(&c)==EEXIST);
    test_client_id=12;
    assert(awdl_delete(&d)==EBUSY && f.removed==0);
    test_client_id=11;
    assert(awdl_delete(&d)==0 && f.removed==1);
    assert(awdl_create(&c)==0 && f.created==2);
    wire.name[0]=0;
    c.req_val=0;test_client_id=12;
    assert(awdl_create(&c)==0 && f.created==2);
    test_client_id=11;
    assert(awdl_delete(&d)==EBUSY && f.removed==1);
    d.req_val=0;
    assert(awdl_delete(&d)==0 && f.removed==2);
    assert(hk_awdl_provider_unregister(generation)==0);
    test_client_id=1;
}
int main(void)
{
    test_control();
    test_diagnostic_ownership();
    test_rollback();
    test_lost_attachment();
    test_blocked_request(CREATE);
    test_blocked_request(CHANNELS);
    test_blocked_request(COPYOUT);
    puts("passed: provider gating, wire ABI, attached name, duplicate create, GET 244, targeted delete, rollback, retirement, stale generation");
}

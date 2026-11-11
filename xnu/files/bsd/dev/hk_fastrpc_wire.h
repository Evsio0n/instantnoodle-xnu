/*
 * FastRPC/GLINK wire contracts for the SDSP
 * runtime.  The structures mirror the public GLINK native framing and the
 * FastRPC invoke metadata layout; no transport or phone access is involved.
 */
#ifndef HACKIOS_FASTRPC_WIRE_H
#define HACKIOS_FASTRPC_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define HK_GLINK_CMD_VERSION             0u
#define HK_GLINK_CMD_VERSION_ACK         1u
#define HK_GLINK_CMD_OPEN                2u
#define HK_GLINK_CMD_CLOSE               3u
#define HK_GLINK_CMD_OPEN_ACK            4u
#define HK_GLINK_CMD_INTENT              5u
#define HK_GLINK_CMD_RX_DONE             6u
#define HK_GLINK_CMD_RX_INTENT_REQ       7u
#define HK_GLINK_CMD_RX_INTENT_REQ_ACK   8u
#define HK_GLINK_CMD_TX_DATA             9u
#define HK_GLINK_CMD_CLOSE_ACK           11u
#define HK_GLINK_CMD_TX_DATA_CONT        12u
#define HK_GLINK_CMD_READ_NOTIF          13u
#define HK_GLINK_CMD_RX_DONE_W_REUSE     14u
#define HK_GLINK_CMD_SIGNALS             15u

#define HK_FASTRPC_CHANNEL "fastrpcglink-apps-dsp"
#define HK_FASTRPC_CHANNEL_ID 3u
#define HK_FASTRPC_CHANNEL_INTENTS 64u
#define HK_FASTRPC_CHANNEL_INTENT_SIZE 100u

#define HK_FASTRPC_SMMU_STREAM 0x541u
#define HK_FASTRPC_IOVA_BASE UINT64_C(0x80000000)
#define HK_FASTRPC_IOVA_SIZE UINT64_C(0x78000000)
#define HK_FASTRPC_IOVA_END (HK_FASTRPC_IOVA_BASE + HK_FASTRPC_IOVA_SIZE)

#define HK_FASTRPC_SENSORS_PD 2u
#define HK_FASTRPC_INIT_HANDLE 1u
#define HK_FASTRPC_LISTENER_HANDLE 3u
#define HK_FASTRPC_APPS_REMOTECTL_HANDLE 0u
#define HK_FASTRPC_INIT_ATTACH_METHOD 0u
#define HK_FASTRPC_LISTENER_INIT2_METHOD 3u
#define HK_FASTRPC_LISTENER_NEXT2_METHOD 4u

#define HK_FASTRPC_ALIGN 128u
#define HK_FASTRPC_MAX_FDLIST 16u
#define HK_FASTRPC_MAX_CRCLIST 64u

/* qcom_glink_native.c's control header.  All fields are little endian. */
struct hk_glink_msg {
	uint16_t cmd;
	uint16_t param1;
	uint32_t param2;
} __attribute__((packed));

struct hk_glink_intent {
	uint16_t id;
	uint16_t lcid;
	uint32_t count;
	uint32_t size;
	uint32_t liid;
} __attribute__((packed));

/* Linux FastRPC's rpmsg invoke descriptor (40 bytes on arm64). */
struct hk_fastrpc_invoke {
	int32_t client_id;
	int32_t tid;
	uint64_t ctx;
	uint32_t handle;
	uint32_t scalars;
	uint64_t addr;
	uint64_t size;
} __attribute__((packed));

/* Response sent on the rpmsg endpoint after the DSP has consumed the DMA. */
struct hk_fastrpc_reply {
	uint64_t ctx;
	int32_t retval;
	uint32_t reserved;
} __attribute__((packed));

struct hk_fastrpc_remote_buf {
	uint64_t pv;
	uint64_t len;
} __attribute__((packed));

struct hk_fastrpc_invoke_buf {
	uint32_t num;
	uint32_t pgidx;
} __attribute__((packed));

struct hk_fastrpc_phy_page {
	uint64_t addr;
	uint64_t size;
} __attribute__((packed));

_Static_assert(sizeof(struct hk_glink_msg) == 8, "GLINK header size");
_Static_assert(sizeof(struct hk_glink_intent) == 16, "GLINK intent size");
_Static_assert(sizeof(struct hk_fastrpc_invoke) == 40, "FastRPC invoke size");
_Static_assert(sizeof(struct hk_fastrpc_reply) == 16, "FastRPC reply size");
_Static_assert(sizeof(struct hk_fastrpc_remote_buf) == 16, "remote arg size");
_Static_assert(sizeof(struct hk_fastrpc_invoke_buf) == 8, "invoke buf size");
_Static_assert(sizeof(struct hk_fastrpc_phy_page) == 16, "page info size");

static inline uint32_t
hk_fastrpc_scalars(unsigned method, unsigned in_bufs, unsigned out_bufs)
{
	return ((method & 0x1fu) << 24) |
	    ((in_bufs & 0xffu) << 16) |
	    ((out_bufs & 0xffu) << 8);
}

static inline uint32_t
hk_fastrpc_scalars_method(uint32_t scalars)
{
	return (scalars >> 24) & 0x1fu;
}

static inline unsigned
hk_fastrpc_scalars_in(uint32_t scalars)
{
	return (scalars >> 16) & 0xffu;
}

static inline unsigned
hk_fastrpc_scalars_out(uint32_t scalars)
{
	return (scalars >> 8) & 0xffu;
}

static inline uint64_t
hk_fastrpc_ctx(unsigned sequence, unsigned slot, unsigned pd)
{
	/* adsprpc's ctxtable index occupies bits 4..11. */
	return ((uint64_t)sequence << 12) | ((slot & 0xffu) << 4) | (pd & 0xfu);
}

static inline unsigned
hk_fastrpc_ctx_pd(uint64_t ctx)
{
	return (unsigned)(ctx & 0xfu);
}

static inline unsigned
hk_fastrpc_ctx_sequence(uint64_t ctx)
{
	return (unsigned)(ctx >> 12);
}

static inline uint64_t
hk_fastrpc_iova_tag(uint64_t iova, unsigned stream_id)
{
	return iova | ((uint64_t)(stream_id & 0xfu) << 32);
}

static inline bool
hk_fastrpc_iova_valid(uint64_t iova)
{
	return iova >= HK_FASTRPC_IOVA_BASE && iova < HK_FASTRPC_IOVA_END;
}

static inline size_t
hk_glink_open_encode(uint8_t *out, size_t cap, uint16_t lcid,
    const char *name)
{
	struct hk_glink_msg msg;
	const size_t n = strlen(name) + 1u;
	const size_t wire = (sizeof(msg) + n + 7u) & ~7u;
	if (n > 32u || wire > cap)
		return 0;
	msg.cmd = HK_GLINK_CMD_OPEN;
	msg.param1 = lcid;
	msg.param2 = (uint32_t)n;
	memset(out, 0, wire);
	memcpy(out, &msg, sizeof(msg));
	memcpy(out + sizeof(msg), name, n);
	return wire;
}

static inline bool
hk_glink_open_decode(const uint8_t *packet, size_t len, uint16_t *lcid,
    const char **name, size_t *name_len)
{
	const struct hk_glink_msg *msg;
	uint32_t n;
	if (len < sizeof(*msg))
		return false;
	msg = (const struct hk_glink_msg *)packet;
	n = msg->param2;
	if (msg->cmd != HK_GLINK_CMD_OPEN || n == 0 || n > 32 ||
	    sizeof(*msg) + n > len || packet[sizeof(*msg) + n - 1] != '\0')
		return false;
	if (lcid)
		*lcid = msg->param1;
	if (name)
		*name = (const char *)(packet + sizeof(*msg));
	if (name_len)
		*name_len = n - 1;
	return true;
}

static inline bool
hk_glink_open_ack_decode(const uint8_t *packet, size_t len,
    uint16_t *remote_cid)
{
	const struct hk_glink_msg *msg;
	if (len < sizeof(*msg))
		return false;
	msg = (const struct hk_glink_msg *)packet;
	if (msg->cmd != HK_GLINK_CMD_OPEN_ACK || msg->param2 != 0)
		return false;
	if (remote_cid)
		*remote_cid = msg->param1;
	return true;
}

static inline bool
hk_glink_intent_decode(const uint8_t *packet, size_t len,
    uint16_t *lcid, uint32_t *count, uint32_t *size, uint32_t *liid)
{
	const struct hk_glink_intent *intent;
	if (len < sizeof(*intent))
		return false;
	intent = (const struct hk_glink_intent *)packet;
	if (intent->id != HK_GLINK_CMD_INTENT || intent->count == 0 ||
	    intent->size == 0)
		return false;
	if (lcid)
		*lcid = intent->lcid;
	if (count)
		*count = intent->count;
	if (size)
		*size = intent->size;
	if (liid)
		*liid = intent->liid;
	return true;
}

static inline size_t
hk_fastrpc_meta_size(unsigned nscalars)
{
	return nscalars * (sizeof(struct hk_fastrpc_remote_buf) +
	    sizeof(struct hk_fastrpc_invoke_buf) +
	    sizeof(struct hk_fastrpc_phy_page)) +
	    HK_FASTRPC_MAX_FDLIST * sizeof(uint64_t) +
	    HK_FASTRPC_MAX_CRCLIST * sizeof(uint32_t) + sizeof(uint32_t);
}

static inline size_t
hk_fastrpc_align(size_t value)
{
	return (value + HK_FASTRPC_ALIGN - 1u) & ~(HK_FASTRPC_ALIGN - 1u);
}

/* Assemble one native ATTACH_SENSORS invocation into an already mapped
 * page. No translation or ownership is established by this encoder. */
static inline bool
hk_fastrpc_encode_attach(struct hk_fastrpc_invoke *message, uint8_t *arena,
    size_t capacity, uint64_t arena_va, uint64_t iova, unsigned stream_id,
    unsigned job, unsigned slot, uint32_t tid, uint32_t tgid)
{
	const size_t page_size = 4096;
	const size_t offset = hk_fastrpc_align(hk_fastrpc_meta_size(1));
	if (!message || !arena || capacity < page_size || !arena_va ||
	    (arena_va & (page_size - 1)) || arena_va > UINT64_MAX - page_size ||
	    (iova & (page_size - 1)) || !hk_fastrpc_iova_valid(iova) ||
	    stream_id < 0x541 || stream_id > 0x543 || slot > 255 ||
	    !tgid || !tid || offset + sizeof(tgid) > page_size)
		return false;

	uint64_t address = hk_fastrpc_iova_tag(iova, stream_id);
	struct hk_fastrpc_remote_buf arg = { arena_va + offset, sizeof(tgid) };
	struct hk_fastrpc_invoke_buf list = { 1, 0 };
	struct hk_fastrpc_phy_page page = { address, page_size };
	memset(arena, 0, page_size);
	memcpy(arena, &arg, sizeof(arg));
	memcpy(arena + sizeof(arg), &list, sizeof(list));
	memcpy(arena + sizeof(arg) + sizeof(list), &page, sizeof(page));
	memcpy(arena + offset, &tgid, sizeof(tgid));
	*message = (struct hk_fastrpc_invoke){
		.client_id = 0, .tid = (int32_t)tid,
		.ctx = hk_fastrpc_ctx(job, slot, HK_FASTRPC_SENSORS_PD),
		.handle = HK_FASTRPC_INIT_HANDLE, .scalars = hk_fastrpc_scalars(0, 1, 0),
		.addr = address, .size = page_size
	};
	return true;
}

/* First listener.next2 call, matching the native zero-capacity discovery
 * pass. It returns the pending callback header and required byte count. */
struct hk_listener_work {
    uint32_t ctx, handle, scalars;
    int32_t required;
};
_Static_assert(sizeof(struct hk_listener_work)==16, "listener primitive output");

static inline size_t hk_listener_output_offset(void)
{
    return hk_fastrpc_align(hk_fastrpc_align(hk_fastrpc_meta_size(4)) + 16);
}

static inline bool hk_fastrpc_encode_next_receive(struct hk_fastrpc_invoke *message,
    uint8_t *arena, size_t capacity, uint64_t arena_va, uint64_t iova, unsigned sid,
    uint32_t tgid, uint32_t tid, unsigned receive_size)
{
    if (!message || !arena || capacity < 4096 || !arena_va || (arena_va & 4095) ||
        arena_va > UINT64_MAX - 4096 || (iova & 4095) || !hk_fastrpc_iova_valid(iova) ||
        sid < 0x541 || sid > 0x543 || !tgid || !tid) return false;
    const size_t in = hk_fastrpc_align(hk_fastrpc_meta_size(4));
    const size_t out = hk_listener_output_offset();
    const size_t payload = hk_fastrpc_align(out+16);
    if (payload>4096 || receive_size>4096-payload) return false;
    uint64_t addr = hk_fastrpc_iova_tag(iova,sid);
    struct hk_fastrpc_remote_buf args[4]={{arena_va+in,16},{0,0},{arena_va+out,16},{receive_size?arena_va+payload:0,receive_size}};
    struct hk_fastrpc_invoke_buf lists[4]={{1,0},{0,1},{1,2},{receive_size?1u:0u,3}};
    struct hk_fastrpc_phy_page pages[4]={{addr,4096},{0,0},{addr,4096},{receive_size?addr:0,receive_size?4096u:0u}};
    const uint32_t input[4]={0,UINT32_MAX,0,receive_size}; /* no previous callback */
    memset(arena,0,4096);
    memcpy(arena,args,sizeof(args));
    memcpy(arena+sizeof(args),lists,sizeof(lists));
    memcpy(arena+sizeof(args)+sizeof(lists),pages,sizeof(pages));
    memcpy(arena+in,input,sizeof(input));
    memset(arena+out,0xff,16); /* detect a missing result before using it */
    *message=(struct hk_fastrpc_invoke){
        .client_id=(int32_t)tgid, .tid=(int32_t)tid,
        .ctx=hk_fastrpc_ctx(3,0,HK_FASTRPC_SENSORS_PD),
        .handle=HK_FASTRPC_LISTENER_HANDLE, .scalars=hk_fastrpc_scalars(4,2,2),
        .addr=addr, .size=4096
    };
    return true;
}

/* 896..2047: incoming callback including its wire framing. 2048..4095:
 * outgoing reply to the previous callback. Neither can overlap descriptors. */
#define HK_LISTENER_RX_CAP 1152u
/* The callback response blob lives beyond the incoming next2 buffer.
 * Copy output before recycling the listener page (foreground owns another page). */
static inline bool
hk_fastrpc_encode_callback_reply(struct hk_fastrpc_invoke *message,uint8_t *page,
    uint32_t tgid,uint32_t tid,uint32_t seq,uint32_t ctx,uint32_t status,
    const uint8_t *reply,unsigned length)
{
    const uint64_t va=(uintptr_t)page;
    const size_t prev=2048;
    if(length>2048 || (length&&!reply) || (status&&length) || seq<6 ||
       !hk_fastrpc_encode_next_receive(message,page,4096,va,HK_FASTRPC_IOVA_BASE,
            0x541,tgid,tid,HK_LISTENER_RX_CAP))return false;
    uint32_t input[4]={ctx,status,length,HK_LISTENER_RX_CAP};
    memcpy(page+640,input,sizeof(input));
    if(length){
        struct hk_fastrpc_remote_buf buf={va+prev,length};
        struct hk_fastrpc_invoke_buf list={1,1};
        struct hk_fastrpc_phy_page phy={message->addr,4096};
        memcpy(page+16,&buf,sizeof(buf));memcpy(page+72,&list,sizeof(list));
        memcpy(page+112,&phy,sizeof(phy));memcpy(page+prev,reply,length);
    }
    message->ctx=hk_fastrpc_ctx(seq,0,HK_FASTRPC_SENSORS_PD);
    return true;
}
static inline bool hk_fastrpc_encode_first_next(struct hk_fastrpc_invoke *m,
    uint8_t *page,size_t cap,uint64_t va,uint64_t iova,unsigned sid,uint32_t tgid,uint32_t tid)
{
    return hk_fastrpc_encode_next_receive(m,page,cap,va,iova,sid,tgid,tid,0);
}

/* Decode only the apps_remotectl.open signature. Length words can be
 * unaligned, particularly the output lengths after the terminating NUL. */
static inline bool
hk_listener_module_name(const uint8_t *buf,size_t n,char name[65])
{
    if (!buf || n<33 || n>256) return false;
    uint32_t prim, named, name_arg, error_max, out_prim, out_error;
    memcpy(&prim,buf,4);memcpy(&named,buf+8,4);
    memcpy(&error_max,buf+12,4);memcpy(&name_arg,buf+16,4);
    if (prim!=8 || named<2 || named>65 || name_arg!=named ||
        n!=24u+named+8 || buf[24+named-1]) return false;
    memcpy(&out_prim,buf+24+named,4);memcpy(&out_error,buf+28+named,4);
    if(out_prim!=8 || out_error!=error_max)return false;
    for(unsigned i=0;i+1<named;i++){
        unsigned c=buf[24+i];
        if(!(c=='_' || (c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
             (c>='0'&&c<='9')))return false;
    }
    memcpy(name,buf+24,named);return true;
}

/* Native SDSP remotectl_open uses a 255-byte diagnostic string. */
#define HK_OPEN_RESULT_OFFSET 896u
static inline bool
hk_fastrpc_encode_default_open(struct hk_fastrpc_invoke *message,uint8_t *page,
    size_t capacity,uint64_t va,uint64_t iova,uint32_t tgid,uint32_t tid)
{
    static const char module[]="adsp_default_listener";
    if (!message || !page || capacity<4096 || !va || (va&4095) || va>UINT64_MAX-4096 ||
        !hk_fastrpc_iova_valid(iova) || (iova&4095) || !tgid || !tid) return false;
    uint64_t addr=hk_fastrpc_iova_tag(iova,0x541);
    const unsigned offsets[]={640,768,HK_OPEN_RESULT_OFFSET,1024};
    const unsigned sizes[]={8,sizeof(module),8,255};
    const uint32_t primitive[]={sizeof(module),255};
    memset(page,0,4096);
    for(unsigned i=0;i<4;i++){
        struct hk_fastrpc_remote_buf arg={va+offsets[i],sizes[i]};
        struct hk_fastrpc_invoke_buf list={1,i};
        struct hk_fastrpc_phy_page entry={addr,4096};
        memcpy(page+i*16,&arg,sizeof(arg));
        memcpy(page+64+i*8,&list,sizeof(list));
        memcpy(page+96+i*16,&entry,sizeof(entry));
    }
    memcpy(page+640,primitive,8);
    memcpy(page+768,module,sizeof(module));
    memset(page+HK_OPEN_RESULT_OFFSET,0xff,8);
    *message=(struct hk_fastrpc_invoke){
        .client_id=(int32_t)tgid,.tid=(int32_t)tid,
        .ctx=hk_fastrpc_ctx(4,1,HK_FASTRPC_SENSORS_PD),
        .handle=0,.scalars=hk_fastrpc_scalars(0,2,2),.addr=addr,.size=4096
    };
    return true;
}

static inline bool
hk_fastrpc_validate_invoke(const struct hk_fastrpc_invoke *msg,
    uint64_t dma_base, uint64_t dma_end)
{
	if (!msg || msg->handle == 0 || msg->size == 0 || msg->addr < dma_base ||
	    msg->addr >= dma_end || msg->size > dma_end - msg->addr)
		return false;
	if (hk_fastrpc_ctx_pd(msg->ctx) > 0xffu ||
	    hk_fastrpc_scalars_in(msg->scalars) +
	    hk_fastrpc_scalars_out(msg->scalars) == 0)
		return false;
	return true;
}

/*
 * The reply carries only the context.  Endpoint/handle ownership is kept by
 * the caller's transaction table, so a reply from an older sequence must not
 * wake the current waiter even when the method is otherwise identical.
 */
static inline bool
hk_fastrpc_reply_matches(const struct hk_fastrpc_invoke *request,
    const struct hk_fastrpc_reply *reply)
{
	return request != NULL && reply != NULL && request->ctx == reply->ctx;
}

#endif

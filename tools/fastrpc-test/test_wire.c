#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../xnu/files/bsd/dev/slpi_dma_arena.h"

static unsigned checks;
static unsigned failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	} \
} while (0)

static void
test_glink_channel(void)
{
	uint8_t packet[128];
	uint16_t cid, remote;
	const char *name;
	size_t name_len, length;
	struct hk_glink_msg ack;

	length = hk_glink_open_encode(packet, sizeof(packet), HK_FASTRPC_CHANNEL_ID,
	    HK_FASTRPC_CHANNEL);
	CHECK(length == 32); /* 8-byte header + 22-byte string, rounded to 32 */
	CHECK(hk_glink_open_decode(packet, length, &cid, &name, &name_len));
	CHECK(cid == HK_FASTRPC_CHANNEL_ID);
	CHECK(name_len == strlen(HK_FASTRPC_CHANNEL));
	CHECK(strcmp(name, HK_FASTRPC_CHANNEL) == 0);

	memset(&ack, 0, sizeof(ack));
	ack.cmd = HK_GLINK_CMD_OPEN_ACK;
	ack.param1 = 11; /* remote channel id learned during OPEN */
	memcpy(packet, &ack, sizeof(ack));
	CHECK(hk_glink_open_ack_decode(packet, sizeof(ack), &remote));
	CHECK(remote == 11);
	ack.param2 = 1;
	memcpy(packet, &ack, sizeof(ack));
	CHECK(!hk_glink_open_ack_decode(packet, sizeof(ack), &remote));
}

static void
test_intents(void)
{
	struct hk_glink_intent intent;
	uint16_t cid;
	uint32_t count, size, id;

	memset(&intent, 0, sizeof(intent));
	intent.id = HK_GLINK_CMD_INTENT;
	intent.lcid = HK_FASTRPC_CHANNEL_ID;
	intent.count = 1;
	intent.size = HK_FASTRPC_CHANNEL_INTENT_SIZE;
	intent.liid = 64;
	CHECK(hk_glink_intent_decode((const uint8_t *)&intent, sizeof(intent),
	    &cid, &count, &size, &id));
	CHECK(cid == HK_FASTRPC_CHANNEL_ID && count == 1 &&
	    size == HK_FASTRPC_CHANNEL_INTENT_SIZE && id == 64);

	/* Device-tree contract: 64 intents, each 100 bytes. */
	CHECK(HK_FASTRPC_CHANNEL_INTENTS == 64);
	CHECK(HK_FASTRPC_CHANNEL_INTENT_SIZE == 100);
	intent.count = 0;
	CHECK(!hk_glink_intent_decode((const uint8_t *)&intent, sizeof(intent),
	    NULL, NULL, NULL, NULL));
}

static void
test_private_page_tables(void)
{
    struct slpi_dma_tables tables;
    /* Distinct nonconsecutive physical pages, including one above 4 GiB. */
    uint64_t pa[] = {0x71001000, 0x31005000, 0x9200c000, UINT64_C(0x100180000)};
    const uint64_t iova = 0x81234000;
    CHECK(slpi_dma_prepare(&tables, pa, iova));
    CHECK(slpi_dma_verify(&tables, pa, iova));
    CHECK(tables.entries[0][2] == (pa[1] | 3));
    CHECK(tables.entries[1][9] == (pa[2] | 3));
    CHECK((tables.entries[2][0x34] & SLPI_DMA_PA_MASK) == pa[3]);
    tables.entries[2][0x35] = tables.entries[2][0x34];
    CHECK(!slpi_dma_verify(&tables, pa, iova)); /* accidental adjacent mapping */
    tables.entries[2][0x35] = 0;
    tables.entries[2][0x34] &= ~(UINT64_C(1) << 54);
    CHECK(!slpi_dma_verify(&tables, pa, iova)); /* executable mapping */
    CHECK(!slpi_dma_prepare(&tables, pa, iova + 1));
    CHECK(!slpi_dma_prepare(&tables, pa, HK_FASTRPC_IOVA_END));
    pa[3] = pa[1];
    CHECK(!slpi_dma_prepare(&tables, pa, iova)); /* DMA page aliases page table */
    pa[3] = UINT64_C(1) << 36;
    CHECK(!slpi_dma_prepare(&tables, pa, iova));
}

static void
test_attach_descriptor(void)
{
	struct hk_fastrpc_invoke msg;
	uint8_t arena[4096];
	uint32_t tgid = 0x12345678;
	CHECK(hk_fastrpc_encode_attach(&msg, arena, sizeof(arena), 0x10000000,
	    0x80000000, 0x541, 1, 7, 17, tgid));
	CHECK(msg.ctx == 0x1072 && msg.scalars == 0x10000 && msg.handle == 1);
	CHECK(msg.addr == UINT64_C(0x180000000) && msg.size == 4096);
	struct hk_fastrpc_remote_buf arg;
	struct hk_fastrpc_phy_page page;
	memcpy(&arg, arena, sizeof(arg));
	memcpy(&page, arena + 24, sizeof(page));
	CHECK(arg.pv == 0x10000200 && arg.len == 4);
	CHECK(page.addr == msg.addr && page.size == 4096);
	CHECK(arena[16] == 1 && arena[20] == 0);
	CHECK(!memcmp(arena + 512, &tgid, 4));
	CHECK(hk_fastrpc_iova_tag(0x80000000, 0x543) == UINT64_C(0x380000000));
	CHECK(!hk_fastrpc_encode_attach(&msg,arena,4095,0x10000000,0x80000000,0x541,1,7,17,tgid));
	CHECK(!hk_fastrpc_encode_attach(&msg,arena,4096,0x10000000,HK_FASTRPC_IOVA_END,0x541,1,7,17,tgid));
}

static void
test_first_next_layout(void)
{
    struct hk_fastrpc_invoke msg;
    uint8_t data[4096];
    uint32_t prim[4];
    struct hk_fastrpc_remote_buf args[4];
    struct hk_fastrpc_invoke_buf lists[4];
    struct hk_fastrpc_phy_page pages[4];
    CHECK(hk_fastrpc_encode_first_next(&msg,data,sizeof(data),0x30000000,0x80000000,0x541,73,42));
    CHECK(msg.ctx==0x3002 && msg.scalars==0x04020200 && msg.handle==3);
    CHECK(msg.client_id==73 && msg.tid==42 && msg.addr==UINT64_C(0x180000000) && msg.size==4096);
    memcpy(args,data,64);memcpy(lists,data+64,32);memcpy(pages,data+96,64);
    CHECK(args[0].pv==0x30000280 && args[0].len==16 && !args[1].len);
    CHECK(args[2].pv==0x30000300 && args[2].len==16 && !args[3].len);
    CHECK(lists[2].num==1 && lists[2].pgidx==2 && lists[3].num==0);
    CHECK(pages[0].addr==msg.addr && pages[2].addr==msg.addr && pages[2].size==4096);
    memcpy(prim,data+640,16);CHECK(!prim[0]&&prim[1]==UINT32_MAX&&!prim[2]&&!prim[3]);
    CHECK(data[768]==0xff && data[783]==0xff && data[784]==0);
    CHECK(!hk_fastrpc_encode_first_next(&msg,data,4095,0x30000000,0x80000000,0x541,73,42));
}

static void
test_concurrent_arguments(void)
{
    uint8_t pages[8192], saved[4096];
    uint64_t pa[]={0x11000,0x33000,0x44000,0x77000,0x22000};
    struct slpi_dma_tables t;
    struct hk_fastrpc_invoke next,open;
    CHECK(slpi_dma_prepare_pages(&t,pa,0x80000000,2));
    CHECK(slpi_dma_verify_pages(&t,pa,0x80000000,2));
    CHECK((t.entries[2][1]&SLPI_DMA_PA_MASK)==pa[4]&&!t.entries[2][2]);
    CHECK(!slpi_dma_prepare_pages(&t,pa,0x801ff000,2));
    CHECK(!slpi_dma_prepare_pages(&t,pa,0xf7fff000,2));
    pa[4]=pa[3];CHECK(!slpi_dma_prepare_pages(&t,pa,0x80000000,2));
    CHECK(hk_fastrpc_encode_first_next(&next,pages,4096,0x40000000,0x80000000,0x541,1,1));
    memcpy(saved,pages,sizeof(saved));
    uint8_t *fg=pages+4096;
    CHECK(hk_fastrpc_encode_default_open(&open,fg,4096,0x40001000,0x80001000,1,2));
    CHECK(!memcmp(saved,pages,4096) && open.ctx==0x4012 && open.handle==0 &&
        open.scalars==0x00020200 && open.addr==next.addr+4096 && open.tid!=next.tid);
    uint32_t in[2];memcpy(in,fg+640,8);
    CHECK(in[0]==22&&in[1]==255&&!memcmp(fg+768,"adsp_default_listener",22));
    struct hk_fastrpc_remote_buf args[4];memcpy(args,fg,sizeof(args));
    CHECK(args[2].pv==0x40001380 && args[2].len==8 && args[3].len==255);
    CHECK(fg[HK_OPEN_RESULT_OFFSET]==0xff);
}

static void
test_buffered_callback(void)
{
    uint8_t request[41]={0},page[4096];
    struct hk_fastrpc_invoke msg;
    CHECK(hk_fastrpc_encode_next_receive(&msg,page,4096,0x30000000,0x80000000,0x541,1,1,256));
    struct hk_fastrpc_remote_buf args[4];memcpy(args,page,sizeof(args));
    CHECK(args[3].pv==0x30000380&&args[3].len==256);
    CHECK(!hk_fastrpc_encode_next_receive(&msg,page,4096,0x30000000,0x80000000,0x541,1,1,4096));
    /* An independent fixture, not the result of an actual callback. */
    request[0]=8;request[8]=9;request[12]=32;request[16]=9;
    memcpy(request+24,"test_mod",9);request[33]=8;request[37]=32;
    char name[65];CHECK(hk_listener_module_name(request,41,name)&&!strcmp(name,"test_mod"));
    CHECK(!hk_listener_module_name(request,40,name));
    request[16]=0xff;CHECK(!hk_listener_module_name(request,41,name));
    request[16]=9;request[32]='x';CHECK(!hk_listener_module_name(request,41,name));
}

static void
test_listener_calls(void)
{
	struct hk_fastrpc_invoke open, init, next;

	memset(&open, 0, sizeof(open));
	open.ctx = hk_fastrpc_ctx(2, 0, HK_FASTRPC_SENSORS_PD);
	open.handle = HK_FASTRPC_APPS_REMOTECTL_HANDLE;
	open.scalars = hk_fastrpc_scalars(0, 2, 1);
	CHECK(open.handle == 0 && hk_fastrpc_scalars_method(open.scalars) == 0);
	CHECK(hk_fastrpc_scalars_in(open.scalars) == 2 &&
	    hk_fastrpc_scalars_out(open.scalars) == 1);

	memset(&init, 0, sizeof(init));
	init.ctx = hk_fastrpc_ctx(3, 0, HK_FASTRPC_SENSORS_PD);
	init.handle = HK_FASTRPC_LISTENER_HANDLE;
	init.scalars = 0x03000000u;
	CHECK(init.handle == 3 && init.scalars == 0x03000000u);
	CHECK(hk_fastrpc_scalars_method(init.scalars) ==
	    HK_FASTRPC_LISTENER_INIT2_METHOD);

	memset(&next, 0, sizeof(next));
	next.ctx = hk_fastrpc_ctx(4, 0, HK_FASTRPC_SENSORS_PD);
	next.handle = HK_FASTRPC_LISTENER_HANDLE;
	next.scalars = 0x04020200u;
	CHECK(next.handle == 3 && next.scalars == 0x04020200u);
	CHECK(hk_fastrpc_scalars_method(next.scalars) ==
	    HK_FASTRPC_LISTENER_NEXT2_METHOD);
	CHECK(hk_fastrpc_scalars_in(next.scalars) == 2 &&
	    hk_fastrpc_scalars_out(next.scalars) == 2);
}

static void
test_stale_reply_guard(void)
{
	struct hk_fastrpc_invoke sent;
	struct hk_fastrpc_reply response;

	memset(&sent, 0, sizeof(sent));
	sent.ctx = hk_fastrpc_ctx(12, 0, HK_FASTRPC_SENSORS_PD);
	sent.handle = HK_FASTRPC_LISTENER_HANDLE;
	sent.scalars = 0x04020200u;
	memset(&response, 0, sizeof(response));
	response.ctx = sent.ctx;
	CHECK(hk_fastrpc_reply_matches(&sent, &response));

	/* A delayed response from a prior sequence cannot complete this call. */
	response.ctx = hk_fastrpc_ctx(11, 0, HK_FASTRPC_SENSORS_PD);
	CHECK(response.ctx != sent.ctx);
	CHECK(hk_fastrpc_ctx_sequence(response.ctx) !=
	    hk_fastrpc_ctx_sequence(sent.ctx));
	CHECK(!hk_fastrpc_reply_matches(&sent, &response));
	/* A late reply with the current context is accepted only by its owner. */
	response.ctx = sent.ctx;
	CHECK(hk_fastrpc_reply_matches(&sent, &response));
}

int
main(void)
{
	test_glink_channel();
	test_intents();
	test_private_page_tables();
	test_attach_descriptor();
	test_buffered_callback();
	test_concurrent_arguments();
	test_listener_calls();
	test_first_next_layout();
	test_stale_reply_guard();
	printf("fastrpc-wire-test: %u checks, %u failures (host-only)\n",
	    checks, failures);
	return failures != 0;
}

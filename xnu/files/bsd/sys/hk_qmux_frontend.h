#ifndef _SYS_HK_QMUX_FRONTEND_H_
#define _SYS_HK_QMUX_FRONTEND_H_
#include <stdbool.h>
#include <stdint.h>
/* Internal kernel client only. No user task gets a connection, shared memory,
 * or a request path. Slots 1 and 2 name qmux1/qmux2. All snapshots are copies.
 * Tokens include a live-pair generation; never reuse one across withdrawal. */
#define HK_QMUX_FRONTEND_TIMEOUT_NS 15000000000ull
struct hk_abp_nas_worker;
struct hk_qmux_channel_info {
    uint64_t generation;
    uint32_t index, node, remote_port, local_port;
    uint8_t cid;
};
struct hk_qmux_nas_answer {
    uint16_t firmware_result, firmware_error, transaction, response_bytes;
    uint8_t registration, cs_attach, ps_attach;
    bool serving_valid;
};
/* A passive NAS Serving System indication. Only registration and the two
 * attach fields leave the decoder. The local port, CID and generation name
 * the recipient; both channels may have the same channel-local CID. */
struct hk_qmux_nas_event {
    struct hk_qmux_channel_info channel;
    uint64_t sequence, received_ns;
    uint8_t registration, cs_attach, ps_attach;
};
enum hk_qmux_subscription_phase {
    HK_QMUX_SUB_STARTING = 1, HK_QMUX_SUB_ACTIVE, HK_QMUX_SUB_REFUSED,
    HK_QMUX_SUB_STOPPING, HK_QMUX_SUB_CLOSED
};
struct hk_qmux_register_ack {
    uint16_t transaction, response_bytes, firmware_result, firmware_error;
};
struct hk_qmux_nas_subscription {
    enum hk_qmux_subscription_phase phase;
    struct hk_qmux_register_ack enable, disable;
    uint64_t sequence_at_ack;
    int terminal_error;
};
#ifdef __cplusplus
extern "C" {
#endif
int hk_qmux_frontend_info(unsigned index, struct hk_qmux_channel_info *);
int hk_qmux_frontend_request(unsigned index, uint64_t generation, uint64_t *token);
int hk_qmux_frontend_take(unsigned index, uint64_t generation, uint64_t token,
                          struct hk_qmux_nas_answer *);
/* Latest addressed indication in this publication. Start after_sequence at
 * zero, then pass the last observed sequence. Reports coalesce; sequence gaps
 * count intervening reports. EAGAIN means no newer report. ESTALE requires
 * reacquiring the generation. All failures clear the output. Success reports
 * what was received at received_ns (monotonic), not a new registration poll. */
int hk_qmux_frontend_read_event(unsigned index, uint64_t generation, uint64_t after_sequence,
                                struct hk_qmux_nas_event *);
/* One explicit enable on this published client. Firmware ACKs, local close
 * and addressed indications are distinct. Status is readable while queued,
 * and the last retired status is retained independently of a replacement.
 * A caller can issue a normal initial GET separately. No enable on open. */
int hk_qmux_frontend_subscribe(unsigned index, uint64_t generation, uint64_t *token);
int hk_qmux_frontend_subscription(unsigned index, uint64_t generation, uint64_t token,
                                  struct hk_qmux_nas_subscription *);
int hk_qmux_frontend_read_subscription_event(unsigned index, uint64_t generation, uint64_t token,
                                             uint64_t after_sequence, struct hk_qmux_nas_event *);
/* Take or cancel any completed GET before unsubscribe (EBUSY). An accepted
 * stop immediately disables scoped reads, even before firmware clears its flag. */
int hk_qmux_frontend_unsubscribe(unsigned index, uint64_t generation, uint64_t token);
int hk_qmux_frontend_cancel(unsigned index, uint64_t generation, uint64_t token);
void hk_qmux_frontend_poll(struct hk_abp_nas_worker *);
bool hk_qmux_frontend_stop(struct hk_abp_nas_worker *);
/* Private IOKit façade. It retains immutable values, never a worker pointer. */
bool hk_qmux_registry_publish(const struct hk_qmux_channel_info *);
bool hk_qmux_registry_withdraw(unsigned index, uint64_t generation);
#ifdef __cplusplus
}
#endif
#endif

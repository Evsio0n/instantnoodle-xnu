/* One in-flight SensorsPD call. The shared DMA page has a separate lifetime
 * from these GLINK packets. RX_DONE acknowledges the packet, not DMA completion. */
struct sg_rpc_call {
    struct hk_fastrpc_invoke message;
    uint32_t peer_id, fill, remaining;
    uint8_t response[64];
    bool prepared, have_peer, offered, sent, tx_done, completed;
    uint32_t early, stale, flags;
    int32_t retval;
};

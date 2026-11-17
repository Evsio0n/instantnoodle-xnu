/* Read Wireshark's pinned synthetic pcap from stdin; also exercise atomic
 * failure and sequence boundaries using local, non-transmitted vectors. */
#include <assert.h>
#include <pcap/pcap.h>
#include <stdio.h>
#include <string.h>
#include "awdl_rx_action.h"

static size_t append_tag(uint8_t *frame, size_t offset, uint8_t tag,
                         const uint8_t *value, size_t length)
{
    frame[offset++] = tag;
    frame[offset++] = (uint8_t)length;
    frame[offset++] = (uint8_t)(length >> 8);
    memcpy(frame + offset, value, length);
    return offset + length;
}

static void unchanged(const uint8_t *frame, size_t size, enum awdl_rx_result result)
{
    struct awdl_rx_action out;
    uint8_t previous[sizeof(out)];
    memset(&out, 0x5a, sizeof(out));
    memcpy(previous, &out, sizeof(out));
    assert(awdl_rx_decode_action(frame, size, &out) == result);
    assert(memcmp(previous, &out, sizeof(out)) == 0);
}

static void test_boundaries(void)
{
    uint8_t frame[768] = {0xd0};
    const uint8_t header[] = {127, 0, 0x17, 0xf2, 8, 0x10, 3, 0,
                              4, 3, 2, 1, 0xff, 0xff, 0xff, 0xff};
    memcpy(frame + 24, header, sizeof(header));
    for (size_t length = 0; length < 40; length++)
        unchanged(frame, length, AWDL_RX_OTHER);
    assert(awdl_rx_decode_action(frame, 40, NULL) == AWDL_RX_INVALID);
    assert(awdl_rx_decode_action(NULL, 0, NULL) == AWDL_RX_INVALID);

    struct awdl_rx_action out;
    /* A valid frame with a previous sample's unsent fields clears that data. */
    memset(&out, 0x5a, sizeof(out));
    assert(awdl_rx_decode_action(frame, 40, &out) == AWDL_RX_OK);
    assert(out.kind == LX_AWDL_MIF && out.fields == 0 && out.sequence.count == 0);
    assert(out.phy_tx_us == 0x01020304 && out.target_tx_us == UINT32_MAX);
    const uint8_t version[] = {0x3e, 2};
    const uint8_t opaque[] = {1, 2, 3};
    size_t offset = append_tag(frame, 40, 222, opaque, sizeof(opaque));
    offset = append_tag(frame, offset, 21, version, sizeof(version));
    assert(awdl_rx_decode_action(frame, offset, &out) == AWDL_RX_OK);
    assert(out.fields == AWDL_RX_HAS_VERSION && out.advertised_version == 0x3e);
    assert(out.device_class == 2);

    /* A cut inside the final field does not commit the valid preceding tag. */
    size_t end = append_tag(frame, offset, 24, (uint8_t[40]){0}, 40);
    for (size_t n = offset + 1; n < end; n++)
        unchanged(frame, n, AWDL_RX_INVALID);
    const uint8_t replacement[] = {0x21, 3};
    end = append_tag(frame, offset, 21, replacement, sizeof(replacement));
    assert(awdl_rx_decode_action(frame, end, &out) == AWDL_RX_OK);
    assert(out.duplicate_fields == AWDL_RX_HAS_VERSION);
    assert(out.advertised_version == 0x21 && out.device_class == 3);
    end = append_tag(frame, end, 21, replacement, 1);
    unchanged(frame, end, AWDL_RX_INVALID);

    const uint8_t tags[] = {4, 5, 24, 18, 21};
    const size_t minimum[] = {40, 19, 40, 7, 2};
    uint8_t zero[40] = {0};
    for (unsigned t = 0; t < sizeof(tags); t++) {
        for (size_t n = 0; n < minimum[t]; n++) {
            end = append_tag(frame, offset - 5, tags[t], zero, n);
            unchanged(frame, end, AWDL_RX_INVALID);
        }
    }
    uint8_t private_v1[37] = {1};
    for (size_t n = 19; n < sizeof(private_v1); n++) {
        end = append_tag(frame, 40, 5, private_v1, n);
        unchanged(frame, end, AWDL_RX_INVALID);
    }
    uint8_t channels[6 + 256 * 2] = {255, 3, 1, 4, 0xff, 0xff};
    channels[6 + 255 * 2] = 149;
    channels[6 + 255 * 2 + 1] = 128;
    end = append_tag(frame, 40, 18, channels, sizeof(channels));
    assert(awdl_rx_decode_action(frame, end, &out) == AWDL_RX_OK);
    assert(out.sequence.count == 256 && out.sequence.entries_decoded);
    assert(out.sequence.entries[255].number == 149 && out.sequence.entries[255].flags_or_opclass == 128);
    memset(channels, 0, sizeof(channels));
    memset(frame + 43, 0, sizeof(channels));
    assert(out.sequence.entries[255].number == 149); /* no pointer into the MPDU */
    channels[0] = 255;
    channels[1] = 3;
    end = append_tag(frame, 40, 18, channels, sizeof(channels) - 1);
    unchanged(frame, end, AWDL_RX_INVALID);

    /* HT control extends the MAC header; protected and fragmented frames are excluded. */
    memcpy(frame + 28, header, sizeof(header));
    frame[1] = 0x80;
    assert(awdl_rx_decode_action(frame, 44, &out) == AWDL_RX_OK);
    assert(out.phy_tx_us == 0x01020304);
    frame[22] = 1;
    unchanged(frame, 44, AWDL_RX_OTHER);
    frame[22] = 0;
    frame[1] = 0x40;
    unchanged(frame, 44, AWDL_RX_OTHER);
}

static void check_fixture_sample(unsigned index, const struct awdl_rx_action *out)
{
    assert(out->type == 8 && out->version == 0x10);
    assert(out->phy_tx_us == 100 && out->target_tx_us == 50);
    if (index == 0 || index == 1 || index == 15) {
        const struct awdl_rx_sync *s = &out->sync;
        assert(out->fields & AWDL_RX_HAS_SYNC);
        assert(s->next_aw_channel == 6 && s->master_channel == 44);
        assert(s->tx_down_counter_tu == 1234 && s->aw_period_tu == 16);
        assert(s->action_frame_period_tu == 110 && s->flags == 0x1800);
        assert(s->presence_mode == 4 && s->next_aw_sequence == 1000 && s->ap_beacon_alignment == 0);
        assert(s->sequence.step_count == 3 && s->sequence.fill_channel == 0xffff);
    }
    if (index == 0 || index == 1) {
        assert(out->fields & AWDL_RX_HAS_ELECTION);
        assert(out->election.id == 7 && out->election.distance == 1);
        assert(out->election.master_metric == 600 && out->election.self_metric == 500);
        assert(out->election.private_present == (index == 1));
        assert(out->advertised_version == 0x3e && out->device_class == index + 1);
    }
    if (index == 0) {
        assert(out->kind == LX_AWDL_PSF && out->subtype == 0);
        assert(out->sync.sequence.count == 16 && out->sync.sequence.encoding == AWDL_RX_SIMPLE);
        assert(out->sync.sequence.entries[8].number == 149);
    } else if (index == 1) {
        assert(out->kind == LX_AWDL_MIF && out->subtype == 3);
        assert(out->sync.sequence.count == 4 && out->sync.sequence.encoding == AWDL_RX_LEGACY);
        assert(out->sync.sequence.entries[2].number == 44 && out->sync.sequence.entries[2].flags_or_opclass == 0x1d);
        assert(out->election.private_metric == 650 && out->election.private_id == 99 && out->election.private_phc == 12);
        assert(out->fields & AWDL_RX_HAS_ELECTION_V2);
        assert(out->election_v2.master_counter == 42 && out->election_v2.self_counter == 41);
        assert(out->election_v2.distance == 1 && out->election_v2.master_metric == 600 && out->election_v2.self_metric == 500);
    } else if (index == 2) {
        assert(out->sequence.entries_decoded && out->sequence.count == 3);
        assert(out->sequence.entries[0].number == 6 && out->sequence.entries[0].flags_or_opclass == 0x51);
        assert(out->sequence.entries[1].number == 44 && out->sequence.entries[1].flags_or_opclass == 0x80);
    } else if (index == 3) {
        assert(out->fields & AWDL_RX_HAS_SEQUENCE);
        assert(out->sequence.encoding == 2 && out->sequence.count == 2 && !out->sequence.entries_decoded);
    } else if (index == 15) {
        assert(out->sync.sequence.count == 1 && out->sync.sequence.entries[0].number == 149);
    }
}

int main(void)
{
    test_boundaries();
    char err[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_fopen_offline(stdin, err);
    assert(pcap && pcap_datalink(pcap) == DLT_IEEE802_11);
    struct pcap_pkthdr *header;
    const uint8_t *packet;
    int result;
    unsigned index = 0, decoded = 0, repeated = 0, other = 0;
    while ((result = pcap_next_ex(pcap, &header, &packet)) == 1) {
        assert(header->caplen == header->len);
        struct awdl_rx_action observation;
        enum awdl_rx_result status = awdl_rx_decode_action(packet, header->caplen, &observation);
        if (index == 14 || index >= 16) {
            assert(status == AWDL_RX_OTHER);
            other++;
        } else {
            assert(status == AWDL_RX_OK);
            check_fixture_sample(index, &observation);
            if (index == 12) {
                assert(observation.duplicate_fields == AWDL_RX_HAS_VERSION);
                assert(observation.advertised_version == 0x21 && observation.device_class == 3);
                repeated++;
            } else {
                assert(!observation.duplicate_fields);
            }
            decoded++;
        }
        index++;
    }
    pcap_close(pcap);
    assert(result == -2 && decoded == 15 && repeated == 1 && other == 9 && index == 24);
    printf("typed synthetic samples: decoded=%u with_repeated_fields=%u other=%u result_size=%zu\n",
           decoded, repeated, other, sizeof(struct awdl_rx_action));
    return 0;
}

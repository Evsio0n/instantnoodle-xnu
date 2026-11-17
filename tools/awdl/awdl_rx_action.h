#ifndef HK_AWDL_RX_ACTION_H
#define HK_AWDL_RX_ACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../wifi/lx/src/lx_awdl_rx.h"

/* Decode a complete, immutable, FCS-stripped MPDU. The result owns its bytes.
 * No assignment to out is made unless the complete TLV stream validates.
 * The advertised 32-bit TX times are peer-clock samples, not local RX TSF.
 * Address fields are transient protocol identifiers; do not log them. */
enum awdl_rx_result { AWDL_RX_OK, AWDL_RX_OTHER, AWDL_RX_INVALID };
enum awdl_rx_channel_encoding { AWDL_RX_SIMPLE = 0, AWDL_RX_LEGACY = 1, AWDL_RX_OPCLASS = 3 };
enum awdl_rx_field {
    AWDL_RX_HAS_SYNC = 1u << 0,
    AWDL_RX_HAS_ELECTION = 1u << 1,
    AWDL_RX_HAS_ELECTION_V2 = 1u << 2,
    AWDL_RX_HAS_SEQUENCE = 1u << 3,
    AWDL_RX_HAS_VERSION = 1u << 4
};

struct awdl_rx_channel {
    uint8_t number;
    uint8_t flags_or_opclass;
};

struct awdl_rx_channel_sequence {
    uint16_t count;                 /* one plus the unsigned count octet */
    uint8_t encoding;
    uint8_t duplicate_count;
    uint8_t step_count;             /* raw on-wire value */
    uint16_t fill_channel;
    bool entries_decoded;           /* an unfamiliar encoding has no entries */
    struct awdl_rx_channel entries[256];
};

struct awdl_rx_sync {
    uint8_t next_aw_channel, master_channel, guard_time;
    uint16_t tx_down_counter_tu, aw_period_tu, action_frame_period_tu, flags;
    uint16_t aw_extension_tu, common_aw_tu, remaining_aw_tu;
    uint8_t min_extension, max_multicast_extension, max_unicast_extension, max_af_extension;
    uint8_t master[6];
    uint8_t presence_mode;
    uint16_t next_aw_sequence, ap_beacon_alignment;
    struct awdl_rx_channel_sequence sequence;
};

struct awdl_rx_election {
    uint8_t flags, distance;
    uint16_t id;
    uint8_t master[6];
    uint32_t master_metric, self_metric;
    bool private_present;
    uint8_t private_master[6];
    uint32_t private_metric, private_id;
    uint16_t private_phc;
};

struct awdl_rx_election_v2 {
    uint8_t master[6], sync_address[6];
    uint32_t master_counter, distance, master_metric, self_metric, self_counter;
};

struct awdl_rx_action {
    enum lx_awdl_action kind;
    uint8_t type, version, subtype;
    uint32_t phy_tx_us, target_tx_us;
    unsigned fields;
    unsigned duplicate_fields;      /* bitmask of repeated recognized tags; last occurrence wins */
    struct awdl_rx_sync sync;
    struct awdl_rx_election election;
    struct awdl_rx_election_v2 election_v2;
    struct awdl_rx_channel_sequence sequence; /* standalone type 18 */
    uint8_t advertised_version, device_class; /* type 21 */
};

static inline uint16_t awdl_rx_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t awdl_rx_le32(const uint8_t *p)
{
    return (uint32_t)awdl_rx_le16(p) | ((uint32_t)awdl_rx_le16(p + 2) << 16);
}

static inline unsigned awdl_rx_channel_width(uint8_t encoding)
{
    switch (encoding) {
    case AWDL_RX_SIMPLE: return 1;
    case AWDL_RX_LEGACY:
    case AWDL_RX_OPCLASS: return 2;
    default: return 0;
    }
}

static inline bool awdl_rx_sequence_valid(const uint8_t *p, size_t len)
{
    if (len < 6)
        return false;
    unsigned width = awdl_rx_channel_width(p[1]);
    size_t count = (size_t)p[0] + 1;
    /* An unknown encoding can only be framed by its enclosing TLV.
     * No channel entry is exposed in that case. Known encodings may carry
     * padding at the end, included in the TLV's declared byte length. */
    return count * width <= len - 6;
}

static inline void awdl_rx_read_sequence(const uint8_t *p, struct awdl_rx_channel_sequence *seq)
{
    seq->count = (uint16_t)p[0] + 1;
    seq->encoding = p[1];
    seq->duplicate_count = p[2];
    seq->step_count = p[3];
    seq->fill_channel = awdl_rx_le16(p + 4);
    unsigned width = awdl_rx_channel_width(seq->encoding);
    seq->entries_decoded = width != 0;
    if (!width)
        return;
    p += 6;
    for (unsigned i = 0; i < seq->count; i++, p += width) {
        if (seq->encoding == AWDL_RX_LEGACY) {
            seq->entries[i].number = p[1];
            seq->entries[i].flags_or_opclass = p[0];
        } else {
            seq->entries[i].number = p[0];
            if (seq->encoding == AWDL_RX_OPCLASS)
                seq->entries[i].flags_or_opclass = p[1];
        }
    }
}

static inline void awdl_rx_address(uint8_t out[6], const uint8_t *p)
{
    for (unsigned i = 0; i < 6; i++)
        out[i] = p[i];
}

static inline void awdl_rx_read_sync(const uint8_t *p, struct awdl_rx_sync *sync)
{
    sync->next_aw_channel = p[0];
    sync->tx_down_counter_tu = awdl_rx_le16(p + 1);
    sync->master_channel = p[3];
    sync->guard_time = p[4];
    sync->aw_period_tu = awdl_rx_le16(p + 5);
    sync->action_frame_period_tu = awdl_rx_le16(p + 7);
    sync->flags = awdl_rx_le16(p + 9);
    sync->aw_extension_tu = awdl_rx_le16(p + 11);
    sync->common_aw_tu = awdl_rx_le16(p + 13);
    sync->remaining_aw_tu = awdl_rx_le16(p + 15);
    sync->min_extension = p[17];
    sync->max_multicast_extension = p[18];
    sync->max_unicast_extension = p[19];
    sync->max_af_extension = p[20];
    awdl_rx_address(sync->master, p + 21);
    sync->presence_mode = p[27];
    sync->next_aw_sequence = awdl_rx_le16(p + 29);
    sync->ap_beacon_alignment = awdl_rx_le16(p + 31);
    awdl_rx_read_sequence(p + 33, &sync->sequence);
}

static inline void awdl_rx_read_election(const uint8_t *p, struct awdl_rx_election *election)
{
    election->flags = p[0];
    election->id = awdl_rx_le16(p + 1);
    election->distance = p[3];
    awdl_rx_address(election->master, p + 5);
    election->master_metric = awdl_rx_le32(p + 11);
    election->self_metric = awdl_rx_le32(p + 15);
    election->private_present = p[0] != 0;
    if (election->private_present) {
        awdl_rx_address(election->private_master, p + 21);
        election->private_metric = awdl_rx_le32(p + 27);
        election->private_id = awdl_rx_le32(p + 31);
        election->private_phc = awdl_rx_le16(p + 35);
    }
}

static inline void awdl_rx_read_election_v2(const uint8_t *p, struct awdl_rx_election_v2 *election)
{
    awdl_rx_address(election->master, p);
    awdl_rx_address(election->sync_address, p + 6);
    election->master_counter = awdl_rx_le32(p + 12);
    election->distance = awdl_rx_le32(p + 16);
    election->master_metric = awdl_rx_le32(p + 20);
    election->self_metric = awdl_rx_le32(p + 24);
    election->self_counter = awdl_rx_le32(p + 36);
}

static inline enum awdl_rx_result
awdl_rx_decode_action(const uint8_t *frame, size_t length, struct awdl_rx_action *out)
{
    /* Index only these five TLVs before committing. No packet-sized stack
     * temporary, borrowed references in the output, or allocator is needed. */
    const uint8_t *sync = NULL, *v1 = NULL, *v2 = NULL, *sequence = NULL, *version = NULL;
    unsigned duplicates = 0;
    if (!out)
        return AWDL_RX_INVALID;
    enum lx_awdl_action kind = lx_awdl_action_kind(frame, length);
    if (kind == LX_AWDL_NONE)
        return AWDL_RX_OTHER;
    size_t header = (frame[1] & 0x80) ? 28 : 24;
    const uint8_t *action = frame + header;
    size_t offset = header + 16;
    while (offset < length) {
        if (length - offset < 3)
            return AWDL_RX_INVALID;
        uint8_t tag = frame[offset];
        size_t len = awdl_rx_le16(frame + offset + 1);
        offset += 3;
        if (len > length - offset)
            return AWDL_RX_INVALID;
        const uint8_t *p = frame + offset;
        offset += len;
        switch (tag) {
        case 4:
            if (len < 33 || !awdl_rx_sequence_valid(p + 33, len - 33))
                return AWDL_RX_INVALID;
            if (sync)
                duplicates |= AWDL_RX_HAS_SYNC;
            sync = p;
            break;
        case 5:
            if (len < 19 || (p[0] && len < 37))
                return AWDL_RX_INVALID;
            if (v1)
                duplicates |= AWDL_RX_HAS_ELECTION;
            v1 = p;
            break;
        case 18:
            if (!awdl_rx_sequence_valid(p, len))
                return AWDL_RX_INVALID;
            if (sequence)
                duplicates |= AWDL_RX_HAS_SEQUENCE;
            sequence = p;
            break;
        case 21:
            if (len < 2)
                return AWDL_RX_INVALID;
            if (version)
                duplicates |= AWDL_RX_HAS_VERSION;
            version = p;
            break;
        case 24:
            if (len < 40)
                return AWDL_RX_INVALID;
            if (v2)
                duplicates |= AWDL_RX_HAS_ELECTION_V2;
            v2 = p;
            break;
        default:
            break;
        }
    }
    *out = (struct awdl_rx_action){0};
    out->kind = kind;
    out->duplicate_fields = duplicates;
    out->type = action[4];
    out->version = action[5];
    out->subtype = action[6];
    out->phy_tx_us = awdl_rx_le32(action + 8);
    out->target_tx_us = awdl_rx_le32(action + 12);
    if (sync) {
        out->fields |= AWDL_RX_HAS_SYNC;
        awdl_rx_read_sync(sync, &out->sync);
    }
    if (v1) {
        out->fields |= AWDL_RX_HAS_ELECTION;
        awdl_rx_read_election(v1, &out->election);
    }
    if (v2) {
        out->fields |= AWDL_RX_HAS_ELECTION_V2;
        awdl_rx_read_election_v2(v2, &out->election_v2);
    }
    if (sequence) {
        out->fields |= AWDL_RX_HAS_SEQUENCE;
        awdl_rx_read_sequence(sequence, &out->sequence);
    }
    if (version) {
        out->fields |= AWDL_RX_HAS_VERSION;
        out->advertised_version = version[0];
        out->device_class = version[1];
    }
    return AWDL_RX_OK;
}

#endif

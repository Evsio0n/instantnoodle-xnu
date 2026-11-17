#ifndef LX_AWDL_DISCOVERY_H
#define LX_AWDL_DISCOVERY_H
#include "psf.h"
#include <stdbool.h>

/* Normal local PSF/MIF builder for a real channel-6 VIF. The owner supplies
 * one monotonic clock domain and an actual continuous channel grant. Slots
 * outside the grant are idle. No remote identities or service records enter
 * the builder. OWL tx.c:362-380 defines the PSF/MIF TLV order. */
struct lx_awdl_discovery_window {
    uint64_t now_us, anchor_us, grant_end_us;
    uint16_t ieee_sequence;       /* actual next 12-bit management sequence */
    uint16_t af_period_tu;        /* actual interval selected by the owner */
    uint16_t base_aw;             /* observed master AW at anchor, zero for local startup */
    uint8_t channel;              /* actual granted channel, zero keeps startup channel 6 */
    uint8_t master_channel;
    uint16_t social_mask;
    bool dualband_supported;     /* both bands exist in the live eligible wiphy */
    bool follow_master;
    uint8_t master[6], sync_address[6];
    uint32_t master_counter, master_metric, distance;
};

struct lx_awdl_discovery_ht {
    uint16_t capabilities;        /* live channel-6 wiphy HT capabilities */
    uint8_t ampdu_parameters;
    uint8_t rx_mcs0;              /* live supported RX MCS mask, first octet */
};

#define LX_AWDL_DISCOVERY_PSF_LENGTH 259u
#define LX_AWDL_DISCOVERY_MAX 289u

static inline void
lx_awdl_discovery_sequence(uint8_t *seq, unsigned current_slot, unsigned slots, unsigned channel)
{
    seq[0] = 15;
    seq[1] = 3; /* unsuppressed opclass-encoded sequence */
    seq[3] = 3; /* four 16-TU AW per EAW */
    psf_le16(seq + 4, 0xffff);
    for (unsigned i = 0; i < slots; i++) {
        unsigned entry = 6 + 2 * ((current_slot + i) & 15);
        seq[entry] = (uint8_t)channel;
        seq[entry + 1] = channel<=14?81:channel<=48?115:channel<=64?118:channel<=144?121:channel<=161?124:125;
    }
}

static inline size_t
lx_awdl_discovery_action(uint8_t *out, size_t capacity, const uint8_t self[6],
                         uint8_t subtype, const struct lx_awdl_discovery_window *w,
                         const struct lx_awdl_discovery_ht *ht)
{
    static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
    static const char label[] = "hackios-awdl";
    const size_t label_len = sizeof(label) - 1;
    const size_t length = LX_AWDL_DISCOVERY_PSF_LENGTH +
        (subtype == 3 ? 7 + label_len + (ht ? 11 : 0) : 0);
    if (!out || !self || !w || (subtype != 0 && subtype != 3) ||
        capacity < length || !w->anchor_us || w->now_us < w->anchor_us ||
        w->grant_end_us <= w->now_us || w->ieee_sequence > 0xfff || !w->af_period_tu)
        return 0;
    if ((self[0] & 1) || (!(self[0] & ~2u) &&
        !(self[1] | self[2] | self[3] | self[4] | self[5])))
        return 0;
    if (w->channel>177 || (w->base_aw&3) || (w->follow_master &&
        ((w->master[0]&1) || (w->sync_address[0]&1) || !w->distance || w->distance>=10 ||
         !(w->master[0]|w->master[1]|w->master[2]|w->master[3]|w->master[4]|w->master[5]) ||
         !(w->sync_address[0]|w->sync_address[1]|w->sync_address[2]|w->sync_address[3]|w->sync_address[4]|w->sync_address[5])))) return 0;

    const uint64_t elapsed = w->now_us - w->anchor_us;
    const uint64_t remainder = 65536u - elapsed % 65536u;
    const uint64_t grant = w->grant_end_us - w->now_us;
    if (grant < remainder)
        return 0; /* cannot claim attendance through the end of this EAW */
    uint64_t extra = (grant - remainder) / 65536u;
    unsigned slots = extra >= 15 ? 16 : (unsigned)extra + 1;
    unsigned channel=w->channel?w->channel:6;
    unsigned current_slot = (unsigned)((w->base_aw/4u + elapsed / 65536u) & 15u);
    unsigned phase_tu = (unsigned)((elapsed / 1024u) % 64u);
    uint16_t aw = (uint16_t)(w->base_aw + elapsed / 16384u);
    for (size_t i = 0; i < length; i++)
        out[i] = 0;
    out[0] = 0xd0;
    for (unsigned i = 0; i < 6; i++)
        out[4 + i] = 0xff;
    psf_addr(out + 10, self);
    psf_addr(out + 16, bssid);
    psf_le16(out + 22, (unsigned)w->ieee_sequence << 4);
    out[24] = 127;
    out[26] = 0x17;
    out[27] = 0xf2;
    out[28] = 8;
    out[29] = 0x10;
    out[30] = subtype;
    psf_le32(out + 32, (uint32_t)w->now_us);
    psf_le32(out + 36, (uint32_t)w->now_us);

    uint8_t *at = out + 40;
    uint8_t *sync = psf_tlv(&at, 4, 73);
    sync[0] = (uint8_t)channel;
    psf_le16(sync + 1, 64 - phase_tu);
    sync[3] = w->master_channel?w->master_channel:(uint8_t)channel;
    psf_le16(sync + 5, 16);
    psf_le16(sync + 7, w->af_period_tu);
    psf_le16(sync + 9, 0x1800);
    psf_le16(sync + 11, 16);
    psf_le16(sync + 13, 16);
    psf_le16(sync + 15, phase_tu < 16 ? 16 - phase_tu : 0);
    for (unsigned i = 17; i <= 20; i++)
        sync[i] = 3;
    psf_addr(sync + 21, w->follow_master?w->master:self);
    sync[27] = 4;
    psf_le16(sync + 29, aw);
    psf_le16(sync + 31, aw);
    lx_awdl_discovery_sequence(sync + 33, current_slot, slots, channel);

    /* An independent, initial local election. No claim of a remote master. */
    uint8_t *v1 = psf_tlv(&at, 5, 21);
    v1[3] = w->follow_master?(uint8_t)w->distance:0;
    psf_addr(v1 + 5, w->follow_master?w->master:self);
    psf_le32(v1 + 11, w->follow_master?w->master_metric:60);
    psf_le32(v1 + 15, w->follow_master?0:60);
    lx_awdl_discovery_sequence(psf_tlv(&at, 18, 40), current_slot, slots, channel);
    uint8_t *v2 = psf_tlv(&at, 24, 40);
    psf_addr(v2, w->follow_master?w->master:self);
    psf_addr(v2 + 6, w->follow_master?w->sync_address:self);
    psf_le32(v2 + 12, w->follow_master?w->master_counter:0);
    psf_le32(v2 + 16, w->follow_master?w->distance:0);
    psf_le32(v2 + 20, w->follow_master?w->master_metric:60);
    psf_le32(v2 + 24, w->follow_master?0:60);
    (void)psf_tlv(&at, 6, 9); /* no advertised services */

    if (subtype == 3) {
        if (ht) {
            uint8_t *cap = psf_tlv(&at, 7, 8);
            psf_le16(cap + 2, ht->capabilities);
            cap[4] = ht->ampdu_parameters & 0x1f;
            cap[5] = ht->rx_mcs0;
        }
        uint8_t *arpa = psf_tlv(&at, 16, (unsigned)(label_len + 4));
        arpa[0] = 3;
        arpa[1] = (uint8_t)label_len;
        for (size_t i = 0; i < label_len; i++)
            arpa[2 + i] = (uint8_t)label[i];
        arpa[2 + label_len] = 0xc0; /* OWL's short .local suffix */
        arpa[3 + label_len] = 0x0c;
    }
    uint8_t *path = psf_tlv(&at, 12, 13);
    /* Wireshark packet-awdl.c: Data Path State bit5 is Dualband Support.
     * Announcing a 5 GHz schedule without this actual radio capability is
     * inconsistent. Do not copy unrelated AirPlay/UMI/RSDB capability bits. */
    /* Native macOS peer state interpreted an omitted country field as
     * the first three bytes of our social-map/address payload (CC=0x07,
     * supported channels missing6). Include the explicit country layout.
     * X0 is OWL's unknown-country encoding; it does not set radio policy. */
    psf_le16(path, 0x0304 | (w->dualband_supported ? 0x0020 : 0));
    path[2]='X';path[3]='0';path[4]=0;
    psf_le16(path + 5, w->social_mask?w->social_mask:channel==6?1:channel==44?2:channel==149?4:0);
    psf_addr(path + 7, self);
    uint8_t *version = psf_tlv(&at, 21, 2);
    version[0] = 0x34; /* OWL's implemented election-v2/opclass profile; action compatibility header remains 1.0 */
    version[1] = 2;    /* iOS */
    return (size_t)(at - out);
}
#endif

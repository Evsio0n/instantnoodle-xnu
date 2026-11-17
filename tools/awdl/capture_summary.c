/* Count headers from an existing capture. No live capture, transmit path,
 * addresses, device names, per-peer tracking, or arbitrary TLV values. */
#include <pcap/pcap.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../wifi/lx/src/lx_awdl_rx.h"
#include "../../wifi/lx/src/lx_awdl_data.h"

struct counts {
    uint64_t packets, psf, mif, other, truncated, bad_header, bad_fcs;
    uint64_t plain_data_ipv6;
    uint64_t complete_tlvs, malformed_tlvs, with_sync, with_election, with_sequence;
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The only optional radiotap field needed for the AWDL TLV boundary is
 * Flags. Follow all present words, then skip TSFT's aligned eight bytes if
 * present. Later radiotap fields do not need interpreting. */
static bool radiotap(const uint8_t *p, size_t n, size_t *offset, uint8_t *flags)
{
    if (n < 8 || p[0] != 0) return false;
    size_t end = le16(p + 2);
    if (end < 8 || end > n) return false;
    uint32_t first = le32(p + 4);
    size_t pos = 8;
    uint32_t word = first;
    while (word & UINT32_C(0x80000000)) {
        if (end - pos < 4) return false;
        word = le32(p + pos);
        pos += 4;
    }
    *flags = 0;
    if (first & 2u) {
        if (first & 1u) {
            pos = (pos + 7u) & ~(size_t)7u;
            if (pos > end || end - pos < 8) return false;
            pos += 8;
        }
        if (pos >= end) return false;
        *flags = p[pos];
    }
    *offset = end;
    return true;
}

/* Count a category only after every length in the entire stream checks out.
 * Content semantics and peer synchronization are not inferred from a tag. */
static void count_tlvs(const uint8_t *p, size_t n, struct counts *c)
{
    bool sync = false, election = false, sequence = false;
    while (n) {
        if (n < 3) { c->malformed_tlvs++; return; }
        size_t len = le16(p + 1);
        if (len > n - 3) { c->malformed_tlvs++; return; }
        switch (p[0]) {
        case 4: sync = true; break;
        case 5: case 24: election = true; break;
        case 18: sequence = true; break;
        }
        n -= len + 3;
        p += len + 3;
    }
    c->complete_tlvs++;
    c->with_sync += sync;
    c->with_election += election;
    c->with_sequence += sequence;
}

static void observe(int link, const struct pcap_pkthdr *h, const uint8_t *packet,
                    bool raw_fcs, struct counts *c)
{
    c->packets++;
    if (h->caplen < h->len) { c->truncated++; return; }
    if (h->caplen != h->len) { c->bad_header++; return; }
    size_t n = h->caplen;
    bool strip_fcs = raw_fcs;
    if (link == DLT_IEEE802_11_RADIO) {
        size_t before;
        uint8_t flags;
        if (!radiotap(packet,n,&before,&flags)) { c->bad_header++; return; }
        if (flags & 0x40) { c->bad_fcs++; return; }
        strip_fcs = (flags & 0x10) != 0;
        packet += before;
        n -= before;
    }
    if (strip_fcs) {
        if (n < 4) { c->bad_header++; return; }
        n -= 4;
    }
    enum lx_awdl_action action = lx_awdl_action_kind(packet,n);
    if (action == LX_AWDL_NONE) {
        c->other++;
        /* This counter covers unfragmented, unprotected three-address
         * non-QoS Data MPDUs. Aggregation and QoS need separate framing. */
        if (n >= 24 && packet[0] == 0x08 && !(packet[1] & 0xc7u)
            && !(packet[22] & 0x0fu)) {
            struct lx_awdl_ipv6 decoded;
            c->plain_data_ipv6 += lx_awdl_ipv6_decap(packet + 24, n - 24, &decoded);
        }
        return;
    }
    if (action == LX_AWDL_PSF) c->psf++;
    else c->mif++;
    size_t start = ((packet[1] & 0x80) ? 28u : 24u) + 16u;
    count_tlvs(packet + start,n - start,c);
}

int main(int argc, char **argv)
{
    bool raw_fcs = argc == 3 && !strcmp(argv[2],"--raw-fcs4");
    if (argc != 2 && !raw_fcs) {
        fputs("usage: capture_summary CAPTURE|- [--raw-fcs4]\n",stderr);
        return 2;
    }
    char err[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(argv[1],err);
    if (!pcap) { fputs("cannot open the offline capture\n",stderr); return 2; }
    int link = pcap_datalink(pcap);
    if ((link != DLT_IEEE802_11 && link != DLT_IEEE802_11_RADIO) ||
        (raw_fcs && link != DLT_IEEE802_11)) {
        fputs("expected native 802.11 or radiotap; explicit FCS only applies to native 802.11\n",stderr);
        pcap_close(pcap);
        return 2;
    }
    struct counts c = {0};
    struct pcap_pkthdr *h;
    const u_char *p;
    int status;
    while ((status = pcap_next_ex(pcap,&h,&p)) >= 0) {
        if (status == 1) observe(link,h,p,raw_fcs,&c);
    }
    pcap_close(pcap);
    if (status != -2) { fputs("incomplete capture file\n",stderr); return 2; }
    printf("{\"datalink\":%d,\"packets\":%" PRIu64 ",\"psf\":%" PRIu64 ",\"mif\":%" PRIu64
           ",\"other\":%" PRIu64 ",\"truncated\":%" PRIu64 ",\"bad_header\":%" PRIu64 ",\"bad_fcs\":%" PRIu64
           ",\"plain_data_ipv6\":%" PRIu64 ",\"complete_tlvs\":%" PRIu64 ",\"malformed_tlvs\":%" PRIu64 ",\"with_sync\":%" PRIu64
           ",\"with_election\":%" PRIu64 ",\"with_sequence\":%" PRIu64 "}\n",
           link,c.packets,c.psf,c.mif,c.other,c.truncated,c.bad_header,c.bad_fcs,c.plain_data_ipv6,c.complete_tlvs,
           c.malformed_tlvs,c.with_sync,c.with_election,c.with_sequence);
    return 0;
}

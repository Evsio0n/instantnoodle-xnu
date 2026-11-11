/*
 * The Pro's network link for the modem's packets (its sys/hk_net.h has units: en0 the radio, en1 the modem). This tree's hk_net.c
 * has one unit (en0, the radio), so the Pro's hk_net.c is here a second time as cell_net.c, every name of it hk_cell_net_*, and only
 * its unit 1 is used: en1. The radio's en0 and its code are untouched.
 */
#ifndef _CELL_NET_H_
#define _CELL_NET_H_
#include <sys/kpi_mbuf.h>

#define HK_NET_FRAME_MAX   1514
#define HK_NET_UNITS       2
#define hk_net_link cell_net_link
struct hk_net_link {
	const char *    name;           /* for the log */
	uint8_t         address[6];     /* the interface's Ethernet address */
	unsigned        unit;           /* which enN this is (0 unless said otherwise) */
	bool            wireless;       /* the interface is told to be Wi-Fi (its "subfamily"), which is how the
	                                 * network stack and configd tell a radio from a cable */
	bool            up;             /* frames can pass from the start (else: wait for hk_net_link_state()) */
	/*
	 * Send one frame: a chain of mbufs, `length` bytes in all, the Ethernet header first. The
	 * packet stays the caller's: copy it (mbuf_copydata()). Returns false if it had to be dropped.
	 * Called by one thread at a time, never from interrupt context.
	 */
	bool            (*transmit)(mbuf_t packet, size_t length);
	/*
	 * Optional. Look whether frames have arrived and call hk_net_input() for each; returns
	 * how many there were. The interface's thread calls it every 2 ms while frames are coming and
	 * every 10 ms when none has for a while. A link that has a thread or an interrupt of its own
	 * leaves it NULL.
	 */
	unsigned        (*poll)(void);
};

bool hk_cell_net_attach(const struct hk_net_link *link);
void hk_cell_net_input_unit(unsigned unit, const uint8_t *frame, size_t length);
void hk_cell_net_link_state_unit(unsigned unit, bool up);
errno_t hk_apple80211_ioctl(u_long command, void *data);
#ifndef CELL_NET_ITSELF
#define hk_net_attach hk_cell_net_attach
#define hk_net_input_unit hk_cell_net_input_unit
#define hk_net_link_state_unit hk_cell_net_link_state_unit
#endif
#endif

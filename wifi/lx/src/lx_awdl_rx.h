/*
 * Recognize an unencrypted AWDL 1.0 synchronization action header in an
 * already-received IEEE 802.11 MPDU. No addresses or payloads are retained.
 * The fixed wire fields are described in OWL src/frame.h and src/rx.c at
 * 0ee3263f944ea721c355f826aa90d636d012a512. The returned kind identifies
 * the header only: discovery, synchronization and IPv6 require more state.
 */
#ifndef LX_AWDL_RX_H
#define LX_AWDL_RX_H

#include <stddef.h>

enum lx_awdl_action {
	LX_AWDL_NONE,
	LX_AWDL_PSF,
	LX_AWDL_MIF
};

static inline enum lx_awdl_action
lx_awdl_action_kind(const unsigned char *frame, size_t length)
{
	if (!frame || length < 24 || frame[0] != 0xd0)
		return LX_AWDL_NONE;
	/* No DS, More Fragments or Protected bits, and fragment number zero.
	 * Order on a management frame adds the four-byte HT Control field. */
	if ((frame[1] & 0x47u) || (frame[22] & 0x0fu))
		return LX_AWDL_NONE;
	size_t header = (frame[1] & 0x80u) ? 28 : 24;
	if (length < header + 16)
		return LX_AWDL_NONE;
	const unsigned char *action = frame + header;
	if (action[0] != 127 || action[1] != 0 || action[2] != 0x17 ||
	    action[3] != 0xf2 || action[4] != 8 || action[5] != 0x10)
		return LX_AWDL_NONE;
	if (action[6] == 0) return LX_AWDL_PSF;
	if (action[6] == 3) return LX_AWDL_MIF;
	return LX_AWDL_NONE;
}
#endif

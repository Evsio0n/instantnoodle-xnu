/* Called only from the existing radio query worker, after its 0x0022 reply.
 * A WDS client for status has no independently bound IP family. Its measured
 * settings are appropriate for a snapshot and cannot authorize an ifnet. */
#ifndef MDM_WDS_READ_H
#define MDM_WDS_READ_H
#include "mdm_wds_settings.h"
#include "mdm_radio_status.h"

struct mdm_wds_projection {
	uint16_t transaction;
	uint32_t detail;
};

static int
mdm_wds_project(const void *data, size_t length, void *output)
{
	struct mdm_wds_projection *projection = output;
	struct mdm_wds_settings settings;
	int error = mdm_wds_decode_current(data, length, projection->transaction, 0, &settings);
	if (error)
		return -error;
	projection->detail = ((uint32_t)settings.family << 16) | settings.mtu;
	return (int)length;
}

/* Splitting the snapshot update from the blocking query permits endpoint and
 * phase callbacks to revoke the first result before settings can be applied.
 * The caller serializes updates and orders samples on its sole radio worker. */
static inline bool
mdm_wds_sample(struct hk_radio_snapshot *s, uint32_t token, int bytes,
               const struct hk_qmi_wds_packet_service_status_resp *status, uint64_t ms)
{
	struct hk_radio_field *field = &s->fields[HK_RADIO_PACKET];
	if (!token || s->phase != HK_RADIO_ACTIVE || field->generation != token ||
	    field->state == HK_RADIO_ABSENT)
		return false;
	field->state = HK_RADIO_QUERYING;
	field->error = field->value = field->detail = 0;
	hk_radio_finish(s, HK_RADIO_PACKET, token, bytes, status->response.result, status->response.error,
	                status->connection_status_valid, status->connection_status, 0, ms);
	return field->state == HK_RADIO_VALUE && field->value == 2 && !status->response.error;
}

static inline void
mdm_wds_publish(struct hk_radio_snapshot *s, uint32_t token, uint64_t sample_ms, uint32_t detail)
{
	struct hk_radio_field *field = &s->fields[HK_RADIO_PACKET];
	if (s->phase == HK_RADIO_ACTIVE && field->generation == token && field->state == HK_RADIO_VALUE &&
	    field->value == 2 && field->observed_ms == sample_ms)
		field->detail = (int32_t)detail;
}

static int
mdm_wds_read_current(struct qmi_handle *handle, const struct sockaddr_qrtr *source, uint32_t *detail,
                     uint64_t timeout_ms)
{
	static const struct qmi_elem_info request[] = {{QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x10, 0, NULL},
	                                               {QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL}};
	/* QmiWdsRequestedSettings IP_ADDRESS | MTU | IP_FAMILY. */
	const uint32_t mask = (1u << 8) | (1u << 13) | (1u << 15);
	struct mdm_wds_projection projected = {0};
	struct hk_qmi_projection projection = {.decode = mdm_wds_project, .output = &projected};
	struct qmi_txn txn;
	*detail = 0;
	int rc = hk_qmi_txn_init_projected(handle, &txn, &projection);
	if (rc < 0)
		return rc;
	projected.transaction = txn.id;
	struct sockaddr_qrtr peer = *source;
	rc = qmi_send_request(handle, &peer, &txn, MDM_WDS_CURRENT, 7, request, &mask);
	if (rc < 0) {
		qmi_txn_cancel(&txn);
		return rc;
	}
	rc = hk_qmi_txn_wait_ms(&txn, timeout_ms);
	if (rc > 0)
		*detail = projected.detail;
	return rc;
}
#endif

#include <sys/hk_qmi_loc.h>

#ifdef KERNEL
#include <string.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <string.h>
#endif

const struct qmi_elem_info hk_qmi_loc_common_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_loc_common_resp, response), qmi_response_type_v01_ei },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_inject_utc_req_ei[] = {
	{ QMI_UNSIGNED_8_BYTE, 1, sizeof(uint64_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_inject_utc_req, utc_ms), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_loc_inject_utc_req, uncertainty_ms), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_register_events_req_ei[] = {
	{ QMI_UNSIGNED_8_BYTE, 1, sizeof(uint64_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_register_events_req, event_mask), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x11,
	    offsetof(struct hk_qmi_loc_register_events_req, client_type_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x11,
	    offsetof(struct hk_qmi_loc_register_events_req, client_type), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_start_req_ei[] = {
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_start_req, session_id), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x10,
	    offsetof(struct hk_qmi_loc_start_req, recurrence), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x12,
	    offsetof(struct hk_qmi_loc_start_req, intermediate), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x13,
	    offsetof(struct hk_qmi_loc_start_req, interval_ms), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_stop_req_ei[] = {
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_stop_req, session_id), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_start_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_loc_start_resp, response), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_start_resp, session_id_valid), NULL },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_start_resp, session_id), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_position_report_ind_ei[] = {
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_position_report_ind, status_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_loc_position_report_ind, status), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x12,
	    offsetof(struct hk_qmi_loc_position_report_ind, uncertainty_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x12,
	    offsetof(struct hk_qmi_loc_position_report_ind, uncertainty_bits), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x2c,
	    offsetof(struct hk_qmi_loc_position_report_ind, satellites_used_valid), NULL },
	{ QMI_DATA_LEN, 64, sizeof(uint8_t), NO_ARRAY, 0x2c,
	    offsetof(struct hk_qmi_loc_position_report_ind, satellites_used_len), NULL },
	{ QMI_UNSIGNED_2_BYTE, 64, sizeof(uint16_t), VAR_LEN_ARRAY, 0x2c,
	    offsetof(struct hk_qmi_loc_position_report_ind, satellites_used), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static const struct qmi_elem_info hk_qmi_loc_sv_entry_raw_ei[] = {
	{ QMI_UNSIGNED_1_BYTE, 28, sizeof(uint8_t), STATIC_ARRAY, 0,
	    offsetof(struct hk_qmi_loc_sv_entry_raw, bytes), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_loc_sv_info_ind_ei[] = {
	{ QMI_DATA_LEN, 1, sizeof(uint8_t), NO_ARRAY, 0x10,
	    offsetof(struct hk_qmi_loc_sv_info_ind, entries_len), NULL },
	{ QMI_STRUCT, 64, sizeof(struct hk_qmi_loc_sv_entry_raw), VAR_LEN_ARRAY, 0x10,
	    offsetof(struct hk_qmi_loc_sv_info_ind, entries), hk_qmi_loc_sv_entry_raw_ei },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static int
hk_qmi_loc_common(struct qmi_handle *qmi, unsigned message, size_t largest,
	const struct qmi_elem_info *request_ei, const void *request,
	struct hk_qmi_loc_common_resp *response, uint64_t timeout_ms)
{
	struct qmi_txn txn;
	int error;

	if (qmi == NULL || request == NULL || response == NULL) {
		return -EINVAL;
	}
	memset(response, 0, sizeof(*response));
	error = qmi_txn_init(qmi, &txn, hk_qmi_loc_common_resp_ei, response);
	if (error < 0) {
		return error;
	}
	error = qmi_send_request(qmi, NULL, &txn, (int)message, largest,
	    request_ei, request);
	if (error != 0) {
		qmi_txn_cancel(&txn);
		return error;
	}
	return hk_qmi_txn_wait_ms(&txn, timeout_ms);
}

int
hk_qmi_loc_inject_utc(struct qmi_handle *qmi,
	const struct hk_qmi_loc_inject_utc_req *request,
	struct hk_qmi_loc_common_resp *response, uint64_t timeout_ms)
{
	return hk_qmi_loc_common(qmi, HK_QMI_LOC_INJECT_UTC, 64,
	    hk_qmi_loc_inject_utc_req_ei, request, response, timeout_ms);
}

int
hk_qmi_loc_register_events(struct qmi_handle *qmi,
	const struct hk_qmi_loc_register_events_req *request,
	struct hk_qmi_loc_common_resp *response, uint64_t timeout_ms)
{
	return hk_qmi_loc_common(qmi, HK_QMI_LOC_REGISTER_EVENTS, 64,
	    hk_qmi_loc_register_events_req_ei, request, response, timeout_ms);
}

int
hk_qmi_loc_start(struct qmi_handle *qmi, const struct hk_qmi_loc_start_req *request,
	struct hk_qmi_loc_start_resp *response, uint64_t timeout_ms)
{
	struct qmi_txn txn;
	int error;

	if (qmi == NULL || request == NULL || response == NULL) {
		return -EINVAL;
	}
	memset(response, 0, sizeof(*response));
	error = qmi_txn_init(qmi, &txn, hk_qmi_loc_start_resp_ei, response);
	if (error < 0) {
		return error;
	}
	error = qmi_send_request(qmi, NULL, &txn, HK_QMI_LOC_START, 64,
	    hk_qmi_loc_start_req_ei, request);
	if (error != 0) {
		qmi_txn_cancel(&txn);
		return error;
	}
	return hk_qmi_txn_wait_ms(&txn, timeout_ms);
}

int
hk_qmi_loc_stop(struct qmi_handle *qmi,
	const struct hk_qmi_loc_stop_req *request,
	struct hk_qmi_loc_common_resp *response, uint64_t timeout_ms)
{
	return hk_qmi_loc_common(qmi, HK_QMI_LOC_STOP, 32,
	    hk_qmi_loc_stop_req_ei, request, response, timeout_ms);
}

int
hk_qmi_loc_decode_position_report(const void *message, size_t length,
	struct hk_qmi_loc_position_report_ind *indication)
{
	if (message == NULL || indication == NULL) {
		return -EINVAL;
	}
	return hk_qmi_decode(message, length, hk_qmi_loc_position_report_ind_ei, indication);
}

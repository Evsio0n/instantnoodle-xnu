/* Minimal table-driven QMI LOC schema used by the modem client.
 *
 * The tables deliberately expose only fields needed by the current GNSS
 * bring-up. Unknown TLVs remain skippable in hk_qmi_decode(), so the modem
 * can add location fields without making this client retain coordinates.
 */
#ifndef _SYS_HK_QMI_LOC_H_
#define _SYS_HK_QMI_LOC_H_

#include <stdint.h>
#include <sys/hk_qmi.h>

#define HK_QMI_LOC_SERVICE                 16u
#define HK_QMI_LOC_REGISTER_EVENTS         0x0021u
#define HK_QMI_LOC_START                   0x0022u
#define HK_QMI_LOC_STOP                    0x0023u
#define HK_QMI_LOC_POSITION_REPORT         0x0024u
#define HK_QMI_LOC_SV_INFO                 0x0025u
#define HK_QMI_LOC_ENGINE_STATE            0x002bu
#define HK_QMI_LOC_INJECT_UTC              0x0038u

struct hk_qmi_loc_common_resp {
	struct qmi_response_type_v01 response;
};

struct hk_qmi_loc_inject_utc_req {
	uint64_t utc_ms;
	uint32_t uncertainty_ms;
};

struct hk_qmi_loc_register_events_req {
	uint64_t event_mask;
	uint8_t client_type_valid;
	uint32_t client_type;
};

struct hk_qmi_loc_start_req {
	uint8_t session_id;
	uint32_t recurrence;
	uint32_t intermediate;
	uint32_t interval_ms;
};

struct hk_qmi_loc_stop_req {
	uint8_t session_id;
};

struct hk_qmi_loc_start_resp {
	struct qmi_response_type_v01 response;
	uint8_t session_id_valid;
	uint8_t session_id;
};

/* The indication table intentionally keeps float/raw values as integer bits. */
struct hk_qmi_loc_position_report_ind {
	uint8_t status_valid;
	uint32_t status;
	uint8_t uncertainty_valid;
	uint32_t uncertainty_bits;
	uint8_t satellites_used_valid;
	uint32_t satellites_used_len;
	uint16_t satellites_used[64];
};

struct hk_qmi_loc_sv_entry_raw {
	uint8_t bytes[28];
};

struct hk_qmi_loc_sv_info_ind {
	uint32_t entries_len;
	struct hk_qmi_loc_sv_entry_raw entries[64];
};

extern const struct qmi_elem_info hk_qmi_loc_common_resp_ei[];
extern const struct qmi_elem_info hk_qmi_loc_inject_utc_req_ei[];
extern const struct qmi_elem_info hk_qmi_loc_register_events_req_ei[];
extern const struct qmi_elem_info hk_qmi_loc_start_req_ei[];
extern const struct qmi_elem_info hk_qmi_loc_stop_req_ei[];
extern const struct qmi_elem_info hk_qmi_loc_start_resp_ei[];
extern const struct qmi_elem_info hk_qmi_loc_position_report_ind_ei[];
extern const struct qmi_elem_info hk_qmi_loc_sv_info_ind_ei[];

int hk_qmi_loc_inject_utc(struct qmi_handle *,
	const struct hk_qmi_loc_inject_utc_req *, struct hk_qmi_loc_common_resp *,
	uint64_t timeout_ms);
int hk_qmi_loc_register_events(struct qmi_handle *,
	const struct hk_qmi_loc_register_events_req *, struct hk_qmi_loc_common_resp *,
	uint64_t timeout_ms);

/*
 * The first table-backed LOC transaction.  The caller must have connected the
 * QMI handle to a LOC service (usually from qmi_ops.new_server).  The return
 * value is the transaction's decoded byte count, a timeout/transport error,
 * or a negative encoder/transaction error; a modem-level failure is reported
 * in response->response.
 */
int hk_qmi_loc_start(struct qmi_handle *, const struct hk_qmi_loc_start_req *,
	struct hk_qmi_loc_start_resp *, uint64_t timeout_ms);

int hk_qmi_loc_stop(struct qmi_handle *, const struct hk_qmi_loc_stop_req *,
	struct hk_qmi_loc_common_resp *, uint64_t timeout_ms);

/* Decode one position-report indication with the same table used by QMI. */
int hk_qmi_loc_decode_position_report(const void *, size_t,
	struct hk_qmi_loc_position_report_ind *);

#endif /* _SYS_HK_QMI_LOC_H_ */

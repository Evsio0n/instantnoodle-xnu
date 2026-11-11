/* Read-only QMI radio queries used while bringing up the modem. */
#ifndef _SYS_HK_QMI_RADIO_H_
#define _SYS_HK_QMI_RADIO_H_

#include <stdint.h>
#include <sys/hk_qmi.h>

/* Service and message numbers from libqmi's public service descriptions,
 * pinned at commit 3454c2c619d516fd46a08fe16c6eebdee061857d (2026-11-07). */
#define HK_QMI_WDS_SERVICE                    1u
#define HK_QMI_DMS_SERVICE                    2u
#define HK_QMI_NAS_SERVICE                    3u

#define HK_QMI_WDS_GET_PACKET_SERVICE_STATUS  0x0022u
#define HK_QMI_DMS_GET_MODEL                  0x0022u
#define HK_QMI_DMS_GET_OPERATING_MODE         0x002du
#define HK_QMI_NAS_GET_SIGNAL_STRENGTH        0x0020u

/* The zero byte is reserved for the C string terminator. */
#define HK_QMI_DMS_MODEL_MAX                  128u

struct hk_qmi_dms_model_resp {
	struct qmi_response_type_v01 response;
	uint8_t model_valid;
	char model[HK_QMI_DMS_MODEL_MAX];
};

struct hk_qmi_dms_operating_mode_resp {
	struct qmi_response_type_v01 response;
	uint8_t mode_valid;
	uint8_t mode;
	uint8_t offline_reason_valid;
	uint16_t offline_reason;
	uint8_t hardware_restricted_valid;
	uint8_t hardware_restricted;
};

struct hk_qmi_nas_signal_strength_req {
	uint16_t request_mask;
};

struct hk_qmi_nas_signal_strength_resp {
	struct qmi_response_type_v01 response;
	uint8_t signal_strength_valid;
	int8_t signal_strength;
	int8_t radio_interface;
};

struct hk_qmi_wds_packet_service_status_resp {
	struct qmi_response_type_v01 response;
	uint8_t connection_status_valid;
	uint8_t connection_status;
};

extern const struct qmi_elem_info hk_qmi_dms_model_resp_ei[];
extern const struct qmi_elem_info hk_qmi_dms_operating_mode_resp_ei[];
extern const struct qmi_elem_info hk_qmi_nas_signal_strength_req_ei[];
extern const struct qmi_elem_info hk_qmi_nas_signal_strength_resp_ei[];
extern const struct qmi_elem_info hk_qmi_wds_packet_service_status_resp_ei[];

/* Return a decoded byte count, or a negative transport/validation error.  A
 * nonzero response.response.result is a modem-level failure returned in the
 * output and does not turn into a host errno. */
int hk_qmi_dms_get_model(struct qmi_handle *, struct hk_qmi_dms_model_resp *, uint64_t timeout_ms);
int hk_qmi_dms_get_operating_mode(struct qmi_handle *, struct hk_qmi_dms_operating_mode_resp *, uint64_t timeout_ms);
int hk_qmi_nas_get_signal_strength(struct qmi_handle *, const struct hk_qmi_nas_signal_strength_req *,
	struct hk_qmi_nas_signal_strength_resp *, uint64_t timeout_ms);
int hk_qmi_wds_get_packet_service_status(struct qmi_handle *, struct hk_qmi_wds_packet_service_status_resp *,
	uint64_t timeout_ms);

/* Kernel-only summary. The known mask uses the libqmi QmiNasRadioInterface numeric
 * bit positions (0 means a reported NONE interface). An empty list has mask 0. */
#define HK_QMI_NAS_GET_SERVING_SYSTEM 0x0024u
struct hk_qmi_nas_serving_status {
    struct qmi_response_type_v01 response;
    uint16_t known_interfaces;
    uint8_t valid, registration, cs_attach, ps_attach;
    uint8_t interface_count, unknown_interfaces, unrecognized_interfaces;
};
int hk_qmi_nas_get_serving_system(struct qmi_handle *, struct hk_qmi_nas_serving_status *, uint64_t timeout_ms);

#define HK_QMI_UIM_SERVICE 11u
#define HK_QMI_UIM_GET_CARD_STATUS 0x002fu
enum { HK_UIM_LOGICAL_SLOTS=2, HK_UIM_ENUM_OTHER=255, HK_UIM_GW_NONE=0xffff };
enum hk_uim_card_validity {
    HK_UIM_VALIDITY_UNREPORTED, HK_UIM_VALIDITY_INVALID, HK_UIM_VALIDITY_VALID, HK_UIM_VALIDITY_OTHER
};
enum { HK_UIM_CARD_UNKNOWN = 3 };
enum hk_uim_gw_mapping {
    HK_UIM_GW_UNASSIGNED, HK_UIM_GW_MAPPED, HK_UIM_GW_INVALID, HK_UIM_GW_OUTSIDE
};
/* Logical wire slots are zero based; physical slot labels are a separate query.
 * State masks encode the observed public app-state enum for each GW app type. */
struct hk_uim_logical_slot {
    uint8_t card_state, error_valid, error, app_count;
    uint8_t gw_apps, ready_gw_apps, sim_states, usim_states, other_gw_states, validity;
    uint8_t type_states[6]; /* app type 0..5 -> bitmap of observed states 0..7 */
};
struct hk_uim_gw_binding {
    uint16_t index; /* Only MAPPED exposes slot<<8 | app; otherwise FFFF. */
    uint8_t status, app_type, app_state;
};
struct hk_qmi_uim_status {
    struct qmi_response_type_v01 response;
    uint8_t valid, slots;
    uint16_t card_state[3], other_cards;
    uint16_t application_state[8], other_applications;
    uint16_t error_mask, unrecognized_errors;
    uint8_t logical_count;
    struct hk_uim_logical_slot logical[HK_UIM_LOGICAL_SLOTS];
    struct hk_uim_gw_binding gw[2]; /* primary and secondary GW, in that order */
    uint8_t validity_present, validity_count;
    /* Confirmed is the subset with an explicit positive per-slot validity flag. */
    uint16_t present_ready_apps, confirmed_ready_apps;
};
int hk_qmi_uim_get_card_status(struct qmi_handle *,struct hk_qmi_uim_status *,uint64_t);

/* Report the first two physical slots on the IN2010. Identifier bytes are
 * skipped in the wire buffer and are never copied to the diagnostic result. */
struct hk_uim_physical_slot {
	uint32_t card, activity, protocol;
	uint8_t logical_slot, atr_present;
};
struct hk_uim_physical_status {
	struct qmi_response_type_v01 response;
	uint8_t count, info_count, valid, info_valid;
	struct hk_uim_physical_slot slots[2];
};
int hk_qmi_uim_get_physical_slots(struct qmi_handle *, struct hk_uim_physical_status *, uint64_t timeout_ms);

/* WDA Get Data Format for the IP_HW0 PCIe data port. Missing optional values
 * have no negotiated meaning, including a missing aggregation mode. */
#define HK_QMI_WDA_SERVICE 26u
#define HK_QMI_WDA_GET_DATA_FORMAT 0x0021u
enum hk_wda_field { HK_WDA_QOS, HK_WDA_LINK, HK_WDA_UPLINK, HK_WDA_DOWNLINK, HK_WDA_FIELD_COUNT };
struct hk_qmi_wda_format {
	struct qmi_response_type_v01 response;
	uint32_t present;
	uint32_t value[HK_WDA_FIELD_COUNT];
};
int hk_qmi_wda_get_pcie_format(struct qmi_handle *, struct hk_qmi_wda_format *, uint64_t timeout_ms);

/* SDX55 TS service, instance 100 in the IN2010 device tree. */
#define HK_QMI_TS_SERVICE 23u
#define HK_QMI_TS_TEMP_REPORT 0x22u
struct hk_qmi_ts_list {
	struct qmi_response_type_v01 response;
	uint8_t valid, count, has_modem_tsens;
};
struct hk_qmi_ts_report {
	char name[33];
	uint8_t name_valid, kind_valid, temperature_valid, sequence_valid;
	uint32_t kind, temperature_bits, sequence;
};
extern const struct qmi_elem_info hk_qmi_ts_report_ei[];
int hk_qmi_ts_list(struct qmi_handle *, struct hk_qmi_ts_list *, uint64_t timeout_ms);
int hk_qmi_ts_current(struct qmi_handle *, uint32_t seq, struct qmi_response_type_v01 *, uint64_t timeout_ms);
int hk_qmi_ts_millicelsius(uint32_t ieee_bits, int *value);


enum hk_qmi_family_query { HK_FQ_FORMAT, HK_FQ_SET_FAMILY, HK_FQ_STATUS, HK_FQ_SERVING };
/* This object resides in the owner until cancellation or successful collect. */
struct hk_qmi_family_op {
	struct qmi_txn txn;
	struct hk_qmi_projection projection;
	union {
		struct qmi_response_type_v01 family_ack;
		struct hk_qmi_wds_packet_service_status_resp status;
		struct hk_qmi_wda_format format;
		struct hk_qmi_nas_serving_status serving;
	} value;
};
int hk_qmi_family_begin(struct qmi_handle *, struct hk_qmi_family_op *, enum hk_qmi_family_query, uint8_t family);

enum hk_uim_obs_query { HK_UO_REGISTER, HK_UO_SLOTS, HK_UO_EXTENDED, HK_UO_LEGACY, HK_UO_ATR, HK_UO_DMS };
struct hk_uim_obs_scalar {
    uint16_t message,result,error;
    uint32_t mask;
    uint8_t present,atr_length;
};
struct hk_uim_obs_extended {
    struct hk_qmi_uim_status card;
    uint16_t result,error;
    uint8_t extended,validity_present,valid_count,valid[2];
};
struct hk_uim_obs_op {
    struct qmi_txn txn;
    struct hk_qmi_projection projection;
    union {
        struct hk_uim_obs_scalar scalar;
        struct hk_uim_obs_extended ext;
        struct hk_uim_physical_status slots;
        struct hk_qmi_uim_status card;
        struct hk_qmi_dms_operating_mode_resp dms;
    } value;
};
/* Owner-retained asynchronous queries; scalar masks are restricted to normal
 * card/slot status (0x15 or clear), direct ATR slot IDs to 1 or 2. */
int hk_qmi_uim_obs_begin(struct qmi_handle *,struct hk_uim_obs_op *,enum hk_uim_obs_query,uint32_t);
#endif /* _SYS_HK_QMI_RADIO_H_ */

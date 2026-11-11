#include <sys/hk_qmi_radio.h>

#ifdef KERNEL
#include <sys/errno.h>
#include <string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define RADIO_TLV_HEADER 3u

const struct qmi_elem_info hk_qmi_dms_model_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_dms_model_resp, response), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_dms_model_resp, model_valid), NULL },
	{ QMI_STRING, HK_QMI_DMS_MODEL_MAX, sizeof(char), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_dms_model_resp, model), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_dms_operating_mode_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, response), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, mode_valid), NULL },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, mode), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x10,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, offline_reason_valid), NULL },
	{ QMI_UNSIGNED_2_BYTE, 1, sizeof(uint16_t), NO_ARRAY, 0x10,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, offline_reason), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x11,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, hardware_restricted_valid), NULL },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x11,
	    offsetof(struct hk_qmi_dms_operating_mode_resp, hardware_restricted), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_nas_signal_strength_req_ei[] = {
	{ QMI_UNSIGNED_2_BYTE, 1, sizeof(uint16_t), NO_ARRAY, 0x10,
	    offsetof(struct hk_qmi_nas_signal_strength_req, request_mask), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static const struct qmi_elem_info hk_qmi_nas_signal_pair_ei[] = {
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(int8_t), NO_ARRAY, 0,
	    offsetof(struct hk_qmi_nas_signal_strength_resp, signal_strength), NULL },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(int8_t), NO_ARRAY, 0,
	    offsetof(struct hk_qmi_nas_signal_strength_resp, radio_interface), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_nas_signal_strength_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_nas_signal_strength_resp, response), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_nas_signal_strength_resp, signal_strength_valid), NULL },
	{ QMI_STRUCT, 1, sizeof(struct hk_qmi_nas_signal_strength_resp), NO_ARRAY, 0x01,
	    0, hk_qmi_nas_signal_pair_ei },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

const struct qmi_elem_info hk_qmi_wds_packet_service_status_resp_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
	    offsetof(struct hk_qmi_wds_packet_service_status_resp, response), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_wds_packet_service_status_resp, connection_status_valid), NULL },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01,
	    offsetof(struct hk_qmi_wds_packet_service_status_resp, connection_status), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

struct radio_tlv_state {
	bool result_seen;
	bool primary_seen;
	bool offline_reason_seen;
	bool hardware_restricted_seen;
};

static uint16_t
radio_u16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static int
radio_begin(const void *data, size_t length, uint16_t message, struct radio_tlv_state *state,
	uint16_t *result, uint16_t *error, const uint8_t **tlvs, size_t *tlv_length)
{
	const uint8_t *p = data;

	if (data == NULL || state == NULL || result == NULL || error == NULL || tlvs == NULL || tlv_length == NULL ||
	    length < QMI_HEADER_SIZE || p[0] != QMI_RESPONSE || radio_u16(p + 3) != message ||
	    radio_u16(p + 5) != length - QMI_HEADER_SIZE) {
		return -EINVAL;
	}
	memset(state, 0, sizeof(*state));
	*result = 0xffff;
	*error = 0xffff;
	*tlvs = p + QMI_HEADER_SIZE;
	*tlv_length = length - QMI_HEADER_SIZE;
	return 0;
}

static int
radio_common_tlv(const uint8_t *value, size_t length, uint16_t *result, uint16_t *error)
{
	if (length != sizeof(struct qmi_response_type_v01)) {
		return -EBADMSG;
	}
	*result = radio_u16(value);
	*error = radio_u16(value + 2);
	if (*result != QMI_RESULT_SUCCESS_V01 && *result != QMI_RESULT_FAILURE_V01) {
		return -EBADMSG;
	}
	if ((*result == QMI_RESULT_SUCCESS_V01 && *error != QMI_ERR_NONE_V01) ||
	    (*result == QMI_RESULT_FAILURE_V01 && *error == QMI_ERR_NONE_V01)) {
		return -EBADMSG;
	}
	return 0;
}

static int
radio_next(const uint8_t **cursor, size_t *remaining, uint8_t *type, const uint8_t **value, size_t *length)
{
	const uint8_t *p = *cursor;

	if (*remaining < RADIO_TLV_HEADER) {
		return -EBADMSG;
	}
	*type = p[0];
	*length = radio_u16(p + 1);
	if (*length > *remaining - RADIO_TLV_HEADER) {
		return -EBADMSG;
	}
	*value = p + RADIO_TLV_HEADER;
	*cursor = *value + *length;
	*remaining -= RADIO_TLV_HEADER + *length;
	return 0;
}

static int
radio_end(const struct radio_tlv_state *state, uint16_t result, bool primary_seen)
{
	if (!state->result_seen) {
		return -EBADMSG;
	}
	if (result == QMI_RESULT_SUCCESS_V01 && !primary_seen) {
		return -EBADMSG;
	}
	return 0;
}

static int
radio_validate_model(const void *data, size_t length)
{
	const uint8_t *cursor, *value, *tlvs;
	size_t remaining, tlv_length, value_length;
	struct radio_tlv_state state;
	uint16_t result = 0, error = 0;
	uint8_t type;
	int ret;

	ret = radio_begin(data, length, HK_QMI_DMS_GET_MODEL, &state, &result, &error, &tlvs, &tlv_length);
	if (ret != 0) {
		return ret;
	}
	cursor = tlvs;
	remaining = tlv_length;
	while (remaining != 0) {
		ret = radio_next(&cursor, &remaining, &type, &value, &value_length);
		if (ret != 0) {
			return ret;
		}
		if (type == 0x02) {
			if (state.result_seen || (ret = radio_common_tlv(value, value_length, &result, &error)) != 0) {
				return ret != 0 ? ret : -EBADMSG;
			}
			state.result_seen = true;
		} else if (type == 0x01) {
			if (state.primary_seen || value_length == 0 || value_length >= HK_QMI_DMS_MODEL_MAX) {
				return -EBADMSG;
			}
			for (size_t i = 0; i < value_length; i++) {
				if (value[i] == 0) {
					return -EBADMSG;
				}
			}
			state.primary_seen = true;
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	return radio_end(&state, result, state.primary_seen);
}

static int
radio_validate_mode(const void *data, size_t length)
{
	const uint8_t *cursor, *value, *tlvs;
	size_t remaining, tlv_length, value_length;
	struct radio_tlv_state state;
	uint16_t result = 0, error = 0;
	uint8_t type;
	int ret;

	ret = radio_begin(data, length, HK_QMI_DMS_GET_OPERATING_MODE, &state, &result, &error, &tlvs, &tlv_length);
	if (ret != 0) {
		return ret;
	}
	cursor = tlvs;
	remaining = tlv_length;
	while (remaining != 0) {
		ret = radio_next(&cursor, &remaining, &type, &value, &value_length);
		if (ret != 0) {
			return ret;
		}
		switch (type) {
		case 0x02:
			if (state.result_seen || (ret = radio_common_tlv(value, value_length, &result, &error)) != 0) {
				return ret != 0 ? ret : -EBADMSG;
			}
			state.result_seen = true;
			break;
		case 0x01:
			if (state.primary_seen || value_length != 1) {
				return -EBADMSG;
			}
			state.primary_seen = true;
			break;
		case 0x10:
			if (state.offline_reason_seen || value_length != 2) {
				return -EBADMSG;
			}
			state.offline_reason_seen = true;
			break;
		case 0x11:
			if (state.hardware_restricted_seen || value_length != 1) {
				return -EBADMSG;
			}
			state.hardware_restricted_seen = true;
			break;
		default:
			if (type < 0x10) {
				return -EBADMSG;
			}
			break;
		}
	}
	return radio_end(&state, result, state.primary_seen);
}

static int
radio_validate_signal(const void *data, size_t length)
{
	const uint8_t *cursor, *value, *tlvs;
	size_t remaining, tlv_length, value_length;
	struct radio_tlv_state state;
	uint16_t result = 0, error = 0;
	uint8_t type;
	int ret;

	ret = radio_begin(data, length, HK_QMI_NAS_GET_SIGNAL_STRENGTH, &state, &result, &error, &tlvs, &tlv_length);
	if (ret != 0) {
		return ret;
	}
	cursor = tlvs;
	remaining = tlv_length;
	while (remaining != 0) {
		ret = radio_next(&cursor, &remaining, &type, &value, &value_length);
		if (ret != 0) {
			return ret;
		}
		if (type == 0x02) {
			if (state.result_seen || (ret = radio_common_tlv(value, value_length, &result, &error)) != 0) {
				return ret != 0 ? ret : -EBADMSG;
			}
			state.result_seen = true;
		} else if (type == 0x01) {
			if (state.primary_seen || value_length != 2) {
				return -EBADMSG;
			}
			state.primary_seen = true;
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	return radio_end(&state, result, state.primary_seen);
}

static int
radio_validate_packet_status(const void *data, size_t length)
{
	const uint8_t *cursor, *value, *tlvs;
	size_t remaining, tlv_length, value_length;
	struct radio_tlv_state state;
	uint16_t result = 0, error = 0;
	uint8_t type;
	int ret;

	ret = radio_begin(data, length, HK_QMI_WDS_GET_PACKET_SERVICE_STATUS, &state, &result, &error, &tlvs, &tlv_length);
	if (ret != 0) {
		return ret;
	}
	cursor = tlvs;
	remaining = tlv_length;
	while (remaining != 0) {
		ret = radio_next(&cursor, &remaining, &type, &value, &value_length);
		if (ret != 0) {
			return ret;
		}
		if (type == 0x02) {
			if (state.result_seen || (ret = radio_common_tlv(value, value_length, &result, &error)) != 0) {
				return ret != 0 ? ret : -EBADMSG;
			}
			state.result_seen = true;
		} else if (type == 0x01) {
			if (state.primary_seen || value_length != 1) {
				return -EBADMSG;
			}
			state.primary_seen = true;
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	return radio_end(&state, result, state.primary_seen);
}

static int
radio_query(struct qmi_handle *qmi, unsigned message, size_t request_largest,
	const struct qmi_elem_info *request_ei, const void *request,
	const struct qmi_elem_info *response_ei, void *response, size_t response_size,
	int (*validate)(const void *, size_t),
	uint64_t timeout_ms)
{
	struct qmi_txn txn;
	int ret;

	if (qmi == NULL || response == NULL) {
		return -EINVAL;
	}
	memset(response, 0, response_size);
	/* A failed validation must never leave a zeroed result looking successful. */
	((struct qmi_response_type_v01 *)response)->result = QMI_RESULT_FAILURE_V01;
	((struct qmi_response_type_v01 *)response)->error = QMI_ERR_INTERNAL_V01;
	ret = qmi_txn_init_checked(qmi, &txn, response_ei, response, validate);
	if (ret < 0) {
		return ret;
	}
	ret = qmi_send_request(qmi, NULL, &txn, (int)message, request_largest, request_ei, request);
	if (ret != 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}
	return hk_qmi_txn_wait_ms(&txn, timeout_ms);
}

int
hk_qmi_dms_get_model(struct qmi_handle *qmi, struct hk_qmi_dms_model_resp *response, uint64_t timeout_ms)
{
	return radio_query(qmi, HK_QMI_DMS_GET_MODEL, 0, NULL, NULL, hk_qmi_dms_model_resp_ei, response,
	    sizeof(*response),
	    radio_validate_model, timeout_ms);
}

int
hk_qmi_dms_get_operating_mode(struct qmi_handle *qmi, struct hk_qmi_dms_operating_mode_resp *response, uint64_t timeout_ms)
{
	return radio_query(qmi, HK_QMI_DMS_GET_OPERATING_MODE, 0, NULL, NULL, hk_qmi_dms_operating_mode_resp_ei, response,
	    sizeof(*response),
	    radio_validate_mode, timeout_ms);
}

int
hk_qmi_nas_get_signal_strength(struct qmi_handle *qmi, const struct hk_qmi_nas_signal_strength_req *request,
	struct hk_qmi_nas_signal_strength_resp *response, uint64_t timeout_ms)
{
	if (request == NULL) {
		return -EINVAL;
	}
	return radio_query(qmi, HK_QMI_NAS_GET_SIGNAL_STRENGTH, 8, hk_qmi_nas_signal_strength_req_ei, request,
	    hk_qmi_nas_signal_strength_resp_ei, response, sizeof(*response), radio_validate_signal, timeout_ms);
}

int
hk_qmi_wds_get_packet_service_status(struct qmi_handle *qmi,
	struct hk_qmi_wds_packet_service_status_resp *response, uint64_t timeout_ms)
{
	return radio_query(qmi, HK_QMI_WDS_GET_PACKET_SERVICE_STATUS, 0, NULL, NULL,
	    hk_qmi_wds_packet_service_status_resp_ei, response, sizeof(*response), radio_validate_packet_status, timeout_ms);
}

/* The selected-network wire byte is skipped when constructing the public summary.
 * An unsigned octet is the entire protocol count. Accept its full representable range. */
struct nas_serving_wire {
    struct qmi_response_type_v01 response;
    uint8_t valid, fields[4];
    uint32_t count;
    int8_t interfaces[255];
};
static const struct qmi_elem_info serving_sequence[] = {
    { QMI_UNSIGNED_1_BYTE, 4, sizeof(uint8_t), STATIC_ARRAY, 0,
        offsetof(struct nas_serving_wire, fields), NULL },
    { QMI_DATA_LEN, 1, sizeof(uint8_t), NO_ARRAY, 0,
        offsetof(struct nas_serving_wire, count), NULL },
    { QMI_UNSIGNED_1_BYTE, 255, sizeof(int8_t), VAR_LEN_ARRAY, 0,
        offsetof(struct nas_serving_wire, interfaces), NULL },
    { QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static const struct qmi_elem_info serving_response[] = {
    { QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02,
        offsetof(struct nas_serving_wire, response), qmi_response_type_v01_ei },
    { QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x01, offsetof(struct nas_serving_wire, valid), NULL },
    { QMI_STRUCT, 1, sizeof(struct nas_serving_wire), NO_ARRAY, 0x01, 0, serving_sequence },
    { QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static int
radio_validate_serving(const void *packet, size_t size)
{
    struct radio_tlv_state state;
    const uint8_t *cursor;
    size_t remaining;
    uint16_t result, error;
    int rc = radio_begin(packet, size, HK_QMI_NAS_GET_SERVING_SYSTEM, &state, &result, &error, &cursor, &remaining);
    if (rc) return rc;
    while (remaining) {
        const uint8_t *v;
        size_t len;
        uint8_t type;
        rc = radio_next(&cursor, &remaining, &type, &v, &len);
        if (rc) return rc;
        if (type == 0x02) {
            if (state.result_seen) return -EBADMSG;
            rc = radio_common_tlv(v, len, &result, &error);
            if (rc) return rc;
            state.result_seen = true;
        } else if (type == 0x01) {
            if (state.primary_seen || len < 5 || len - 5 != v[4]) return -EBADMSG;
            state.primary_seen = true;
        } else if (type < 0x10) {
            return -EBADMSG;
        }
        /* Optional records have been length checked. Never decode their contents. */
    }
    return radio_end(&state, result, state.primary_seen);
}
int
hk_qmi_nas_get_serving_system(struct qmi_handle *qmi, struct hk_qmi_nas_serving_status *out, uint64_t timeout_ms)
{
    struct nas_serving_wire wire = { 0 };
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->response.result = QMI_RESULT_FAILURE_V01;
    out->response.error = QMI_ERR_INTERNAL_V01;
    int rc = radio_query(qmi, HK_QMI_NAS_GET_SERVING_SYSTEM, 0, NULL, NULL,
        serving_response, &wire, sizeof(wire), radio_validate_serving, timeout_ms);
    if (rc <= 0) return rc;
    out->response = wire.response;
    if (wire.response.result != QMI_RESULT_SUCCESS_V01) return rc;
    out->valid = wire.valid;
    out->registration = wire.fields[0];
    out->cs_attach = wire.fields[1];
    out->ps_attach = wire.fields[2];
    out->interface_count = (uint8_t)wire.count;
    for (uint32_t i = 0; i < wire.count; i++) {
        int rat = wire.interfaces[i];
        switch (rat) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 8: case 9: case 12:
            out->known_interfaces |= (uint16_t)(1u << rat);
            break;
        case -1:
            out->unknown_interfaces++;
            break;
        default:
            out->unrecognized_interfaces++;
            break;
        }
    }
    return rc;
}

/* All arrays and references are validated against the same complete TLV. Only
 * non-identifying card/application enums are projected. A bad optional GW
 * reference cannot erase otherwise well-formed historical card histograms. */
static int
uim_card_array(const uint8_t *v, size_t n, struct hk_qmi_uim_status *s,
    const uint16_t indices[2], const uint8_t *validity, unsigned validity_count)
{
    if (!n) return -EBADMSG;
    for(unsigned g=0;g<2;g++){
        s->gw[g].index=HK_UIM_GW_NONE;
        s->gw[g].status=indices[g]==HK_UIM_GW_NONE?HK_UIM_GW_UNASSIGNED:HK_UIM_GW_INVALID;
    }
    s->slots=*v++;n--;
    s->logical_count=s->slots<HK_UIM_LOGICAL_SLOTS?s->slots:HK_UIM_LOGICAL_SLOTS;
    for(unsigned c=0;c<s->slots;c++){
        if(n<6)return -EBADMSG;
        uint8_t state=v[0], apps=v[5];
        enum hk_uim_card_validity quality = HK_UIM_VALIDITY_UNREPORTED;
        if (c < validity_count) {
            quality = validity[c]==0 ? HK_UIM_VALIDITY_INVALID :
                validity[c]==1 ? HK_UIM_VALIDITY_VALID : HK_UIM_VALIDITY_OTHER;
        }
        bool eligible = state==1 && (quality==HK_UIM_VALIDITY_VALID || quality==HK_UIM_VALIDITY_UNREPORTED);
        struct hk_uim_logical_slot *slot=c<s->logical_count?&s->logical[c]:NULL;
        if(slot){
            slot->card_state=state<=HK_UIM_CARD_UNKNOWN?state:HK_UIM_ENUM_OTHER;
            slot->validity=quality;
            slot->app_count=apps;
            if(state==2){slot->error_valid=1;slot->error=v[4]<8?v[4]:HK_UIM_ENUM_OTHER;}
        }
        if(state<3)s->card_state[state]++;else s->other_cards++;
        if(state==2){
            if(v[4]<8)s->error_mask|=(uint16_t)(1u<<v[4]);
            else s->unrecognized_errors++;
        }
        v+=6;n-=6;
        for(unsigned a=0;a<apps;a++){
            if(n<7)return -EBADMSG;
            uint8_t type=v[0], app_state=v[1], aid_length=v[6];
            /* Six fixed bytes, a length-prefixed identifier, seven opaque bytes. */
            size_t bytes=14u+aid_length;
            if(bytes>n)return -EBADMSG;
            if(app_state<8)s->application_state[app_state]++;else s->other_applications++;
            if (slot && type<6 && app_state<8) slot->type_states[type]|=(uint8_t)(1u<<app_state);
            if (eligible && app_state==7) {
                s->present_ready_apps++;
                if (quality==HK_UIM_VALIDITY_VALID) s->confirmed_ready_apps++;
            }
            if(type==1||type==2){ /* SIM, USIM */
                if(slot){
                    slot->gw_apps++;
                    if(eligible&&app_state==7)slot->ready_gw_apps++;
                    if(app_state>=8)slot->other_gw_states++;
                    else if(type==1)slot->sim_states|=(uint8_t)(1u<<app_state);
                    else slot->usim_states|=(uint8_t)(1u<<app_state);
                }
                for(unsigned g=0;g<2;g++){
                    if(indices[g]!=(uint16_t)((c<<8)|a))continue;
                    struct hk_uim_gw_binding *m=&s->gw[g];
                    if(!slot){m->status=HK_UIM_GW_OUTSIDE;continue;}
                    m->status=HK_UIM_GW_MAPPED;m->index=indices[g];
                    m->app_type=type;m->app_state=app_state<8?app_state:HK_UIM_ENUM_OTHER;
                }
            }
            v+=bytes;n-=bytes;
        }
    }
    return n ? -EBADMSG : 0;
}
static int
uim_status_project(const void *data,size_t length,void *output)
{
    struct hk_qmi_uim_status parsed={0};
    struct radio_tlv_state st;
    uint16_t result,error;
    const uint8_t *p;
    size_t n;
    const uint8_t *card_fields=NULL, *validity=NULL;
    size_t card_length=0;
    int rc=radio_begin(data,length,HK_QMI_UIM_GET_CARD_STATUS,&st,&result,&error,&p,&n);
    if(rc)return rc;
    while(n){
        uint8_t type;const uint8_t *v;size_t size;
        rc=radio_next(&p,&n,&type,&v,&size);
        if(rc)return rc;
        if(type==2){
            if(st.result_seen)return -EBADMSG;
            rc=radio_common_tlv(v,size,&result,&error);
            if(rc)return rc;
            st.result_seen=true;
        }else if(type==0x10){
            if(st.primary_seen)return -EBADMSG;
            card_fields=v;
            card_length=size;
            st.primary_seen=true;
        }else if(type==0x12){
            if(parsed.validity_present || !size || v[0]!=size-1)return -EBADMSG;
            parsed.validity_present=1;
            parsed.validity_count=v[0];
            validity=v+1;
        }else if(type<0x10)return -EBADMSG;
    }
    if(!st.result_seen)return -EBADMSG;
    if(st.primary_seen){
        if (card_length<9) return -EBADMSG;
        /* Four little-endian uint16 provisioning references, then the card array. */
        const uint16_t indices[]={radio_u16(card_fields),radio_u16(card_fields+4)};
        rc=uim_card_array(card_fields+8, card_length-8, &parsed, indices, validity, parsed.validity_count);
        if(rc)return rc;
        parsed.valid=1;
    }
    if(result!=QMI_RESULT_SUCCESS_V01)memset(&parsed,0,sizeof(parsed));
    parsed.response.result=result;parsed.response.error=error;
    *(struct hk_qmi_uim_status *)output=parsed;
    return (int)length;
}
int
hk_qmi_uim_get_card_status(struct qmi_handle *qmi,struct hk_qmi_uim_status *s,uint64_t timeout_ms)
{
    struct qmi_txn tx;
    if(!s)return -EINVAL;
    memset(s,0,sizeof(*s));
    s->response.result=QMI_RESULT_FAILURE_V01;s->response.error=QMI_ERR_INTERNAL_V01;
    if(!qmi)return -EINVAL;
    struct hk_qmi_projection projection = { .decode = uim_status_project, .output = s };
    int rc=hk_qmi_txn_init_projected(qmi, &tx, &projection);
    if(rc<0)return rc;
    rc=qmi_send_request(qmi,NULL,&tx,HK_QMI_UIM_GET_CARD_STATUS,0,NULL,NULL);
    if(rc){qmi_txn_cancel(&tx);return rc;}
    return hk_qmi_txn_wait_ms(&tx,timeout_ms);
}

/* libqmi WDA Get Data Format, plus ModemManager's standard Qualcomm PCIe
 * interface number. The interface number is distinct from MHI channels 100/101. */
static const struct qmi_elem_info wda_pcie_request[] = {
	{ QMI_UNSIGNED_4_BYTE, 2, sizeof(uint32_t), STATIC_ARRAY, 0x10, 0, NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static int
wda_project(const void *packet, size_t packet_length, void *output)
{
	const uint8_t *cursor, *value;
	size_t remaining, length;
	struct radio_tlv_state state;
	struct hk_qmi_wda_format format = {0};
	uint16_t result, error;
	uint8_t type;
	int rc = radio_begin(packet, packet_length, HK_QMI_WDA_GET_DATA_FORMAT, &state, &result, &error,
	    &cursor, &remaining);
	if (rc != 0) {
		return rc;
	}
	while (remaining != 0) {
		rc = radio_next(&cursor, &remaining, &type, &value, &length);
		if (rc != 0) {
			return rc;
		}
		if (type == 0x02) {
			if (state.result_seen) {
				return -EBADMSG;
			}
			rc = radio_common_tlv(value, length, &result, &error);
			if (rc != 0) {
				return rc;
			}
			state.result_seen = true;
		} else if (type >= 0x10 && type <= 0x13) {
			unsigned field = type - 0x10;
			uint32_t bit = 1u << field;
			if ((format.present & bit) || length != (field == HK_WDA_QOS ? 1u : 4u)) {
				return -EBADMSG;
			}
			format.present |= bit;
			format.value[field] = value[0];
			if (length == 4) {
				format.value[field] |= (uint32_t)value[1] << 8 |
				    (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
			}
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	if (!state.result_seen) {
		return -EBADMSG;
	}
	if (result != QMI_RESULT_SUCCESS_V01) {
		memset(&format, 0, sizeof(format));
	}
	format.response.result = result;
	format.response.error = error;
	*(struct hk_qmi_wda_format *)output = format;
	return (int)packet_length;
}

int
hk_qmi_wda_get_pcie_format(struct qmi_handle *qmi, struct hk_qmi_wda_format *format, uint64_t timeout_ms)
{
	const uint32_t endpoint[2] = { 3, 4 }; /* PCIe, Qualcomm interface number 4 */
	struct qmi_txn txn;
	struct hk_qmi_projection projection = { .decode = wda_project, .output = format };
	int rc;

	if (!format) {
		return -EINVAL;
	}
	memset(format, 0, sizeof(*format));
	format->response.result = QMI_RESULT_FAILURE_V01;
	format->response.error = QMI_ERR_INTERNAL_V01;
	if (!qmi) {
		return -EINVAL;
	}
	rc = hk_qmi_txn_init_projected(qmi, &txn, &projection);
	if (rc < 0) {
		return rc;
	}
	rc = qmi_send_request(qmi, NULL, &txn, HK_QMI_WDA_GET_DATA_FORMAT, 11, wda_pcie_request, endpoint);
	if (rc != 0) {
		qmi_txn_cancel(&txn);
		return rc;
	}
	return hk_qmi_txn_wait_ms(&txn, timeout_ms);
}

/* The device tree names modem_tsens on the external modem. Only request an
 * immediate reading of that advertised sensor; no high or low trip points. */
static const char ts_modem_name[] = "modem_tsens";

static int
ts_project(const void *packet, size_t size, void *out, uint16_t message)
{
	struct hk_qmi_ts_list list = {0};
	struct radio_tlv_state state;
	uint16_t result, error;
	const uint8_t *p, *v;
	size_t remaining, n;
	uint8_t type;
	int rc = radio_begin(packet, size, message, &state, &result, &error, &p, &remaining);
	if (rc) return rc;
	while (remaining) {
		rc = radio_next(&p, &remaining, &type, &v, &n);
		if (rc) return rc;
		if (type == 2) {
			if (state.result_seen) return -EBADMSG;
			rc = radio_common_tlv(v, n, &result, &error);
			if (rc) return rc;
			state.result_seen = true;
		} else if (message == 0x20 && type == 0x10) {
			if (list.valid || !n || v[0] > 32) return -EBADMSG;
			list.valid = 1;
			list.count = *v++;
			n--;
			for (unsigned i = 0; i < list.count; i++) {
				if (!n || v[0] == 0 || v[0] > 32 || v[0] >= n) return -EBADMSG;
				unsigned length = *v++;
				if (length == sizeof(ts_modem_name) - 1 && !memcmp(v, ts_modem_name, length))
					list.has_modem_tsens = 1;
				v += length;
				n -= length + 1;
			}
			if (n) return -EBADMSG;
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	if (!state.result_seen) return -EBADMSG;
	if (result != QMI_RESULT_SUCCESS_V01) memset(&list, 0, sizeof(list));
	list.response.result = result;
	list.response.error = error;
	if (message == 0x20)
		*(struct hk_qmi_ts_list *)out = list;
	else
		*(struct qmi_response_type_v01 *)out = list.response;
	return (int)size;
}
static int ts_list_project(const void *p, size_t n, void *out) { return ts_project(p, n, out, 0x20); }
static int ts_ack_project(const void *p, size_t n, void *out) { return ts_project(p, n, out, 0x21); }

struct ts_read_request { char name[33]; uint8_t current; uint32_t sequence; };
/* Sensor ID is a nested, length-prefixed string in a STRUCT TLV. */
static const struct qmi_elem_info ts_name_ei[] = {
	{ QMI_STRING, 33, 1, NO_ARRAY, 0, 0, NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static const struct qmi_elem_info ts_current_ei[] = {
	{ QMI_STRUCT, 1, 33, NO_ARRAY, 1, offsetof(struct ts_read_request, name), ts_name_ei },
	{ QMI_UNSIGNED_1_BYTE, 1, 1, NO_ARRAY, 2, offsetof(struct ts_read_request, current), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x12, offsetof(struct ts_read_request, sequence), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
const struct qmi_elem_info hk_qmi_ts_report_ei[] = {
	{ QMI_OPT_FLAG, 1, 1, NO_ARRAY, 1, offsetof(struct hk_qmi_ts_report, name_valid), NULL },
	{ QMI_STRUCT, 1, 33, NO_ARRAY, 1, offsetof(struct hk_qmi_ts_report, name), ts_name_ei },
	{ QMI_OPT_FLAG, 1, 1, NO_ARRAY, 2, offsetof(struct hk_qmi_ts_report, kind_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 2, offsetof(struct hk_qmi_ts_report, kind), NULL },
	{ QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x10, offsetof(struct hk_qmi_ts_report, temperature_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x10, offsetof(struct hk_qmi_ts_report, temperature_bits), NULL },
	{ QMI_OPT_FLAG, 1, 1, NO_ARRAY, 0x11, offsetof(struct hk_qmi_ts_report, sequence_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, 4, NO_ARRAY, 0x11, offsetof(struct hk_qmi_ts_report, sequence), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static int
radio_project_query(struct qmi_handle *qmi, uint16_t message, unsigned max_bytes,
    const struct qmi_elem_info *request_ei, const void *request,
    struct hk_qmi_projection *projection, uint64_t timeout_ms)
{
	struct qmi_txn txn;
	if (!qmi) return -EINVAL;
	int rc = hk_qmi_txn_init_projected(qmi, &txn, projection);
	if (rc < 0) return rc;
	rc = qmi_send_request(qmi, NULL, &txn, message, max_bytes, request_ei, request);
	if (rc) {
		qmi_txn_cancel(&txn);
		return rc;
	}
	return hk_qmi_txn_wait_ms(&txn, timeout_ms);
}

int
hk_qmi_ts_list(struct qmi_handle *qmi, struct hk_qmi_ts_list *out, uint64_t timeout_ms)
{
	if (!out) return -EINVAL;
	*out = (struct hk_qmi_ts_list){ .response = {QMI_RESULT_FAILURE_V01, QMI_ERR_INTERNAL_V01} };
	struct hk_qmi_projection p = { .decode = ts_list_project, .output = out };
	return radio_project_query(qmi, 0x20, 0, NULL, NULL, &p, timeout_ms);
}

int
hk_qmi_ts_current(struct qmi_handle *qmi, uint32_t seq, struct qmi_response_type_v01 *out, uint64_t timeout_ms)
{
	if (!seq || !out) return -EINVAL;
	*out = (struct qmi_response_type_v01){QMI_RESULT_FAILURE_V01, QMI_ERR_INTERNAL_V01};
	struct ts_read_request request = { .name = "modem_tsens", .current = 1, .sequence = seq };
	struct hk_qmi_projection p = { .decode = ts_ack_project, .output = out };
	return radio_project_query(qmi, 0x21, 62, ts_current_ei, &request, &p, timeout_ms);
}

int
hk_qmi_ts_millicelsius(uint32_t bits, int *value)
{
	unsigned biased = (bits >> 23) & 255;
	if (!value || biased > 134) return -ERANGE;
	uint64_t m = (uint64_t)((bits & 0x7fffff) | 0x800000) * 1000;
	unsigned shift = 150 - biased;
	int millic = shift >= 64 ? 0 : (int)(m >> shift);
	if (bits >> 31) millic = -millic;
	if (millic < -50000 || millic > 150000) return -ERANGE;
	*value = millic;
	return 0;
}

static uint32_t
slot_u32(const uint8_t *v)
{
	return (uint32_t)radio_u16(v) | (uint32_t)radio_u16(v + 2) << 16;
}

static int
uim_slot_array(uint8_t type, const uint8_t *v, size_t n, struct hk_uim_physical_status *r)
{
	bool status = type == 0x10;
	if (!n || (status ? r->valid : r->info_valid)) return -EBADMSG;
	unsigned count = *v++;
	n--;
	for (unsigned i = 0; i < count; ++i) {
		unsigned prefix = status ? 10 : 6;
		if (n < prefix) return -EBADMSG;
		/* Status carries length-prefixed ICCID. Information carries length-
		 * prefixed ATR followed by an eUICC flag. Skip both opaque values. */
		unsigned opaque_bytes = v[prefix - 1];
		unsigned size = prefix + opaque_bytes + (status ? 0 : 1);
		if (size > n) return -EBADMSG;
		if (i < sizeof(r->slots) / sizeof(r->slots[0])) {
			if (status) {
				r->slots[i].card = slot_u32(v);
				r->slots[i].activity = slot_u32(v+4);
				r->slots[i].logical_slot = v[8];
			} else {
				r->slots[i].protocol = slot_u32(v);
				r->slots[i].atr_present = opaque_bytes != 0;
			}
		}
		n -= size;
		v += size;
	}
	if (n) return -EBADMSG;
	if (status) {
		r->valid = 1;
		r->count = count;
	} else {
		r->info_valid = 1;
		r->info_count = count;
	}
	return 0;
}

static int
uim_slots_project(const void *packet, size_t length, void *out)
{
	struct hk_uim_physical_status r = {0};
	struct radio_tlv_state state;
	uint16_t result, error;
	const uint8_t *p, *value;
	size_t remaining, bytes;
	uint8_t type;
	int rc = radio_begin(packet,length,0x47,&state,&result,&error,&p,&remaining);
	if (rc) return rc;
	while (remaining) {
		rc = radio_next(&p,&remaining,&type,&value,&bytes);
		if (rc) return rc;
		if (type == 2) {
			if (state.result_seen) return -EBADMSG;
			rc = radio_common_tlv(value,bytes,&result,&error);
			if (rc) return rc;
			state.result_seen = true;
		} else if (type == 0x10 || type == 0x11) {
			rc = uim_slot_array(type,value,bytes,&r);
			if (rc) return rc;
		} else if (type < 0x10) {
			return -EBADMSG;
		}
	}
	if (!state.result_seen || (r.valid && r.info_valid && r.count != r.info_count)) return -EBADMSG;
	if (result != QMI_RESULT_SUCCESS_V01) memset(&r, 0, sizeof(r));
	r.response.result = result;
	r.response.error = error;
	*(struct hk_uim_physical_status *)out = r;
	return (int)length;
}

int
hk_qmi_uim_get_physical_slots(struct qmi_handle *qmi, struct hk_uim_physical_status *out, uint64_t timeout_ms)
{
	if (!out) return -EINVAL;
	*out = (struct hk_uim_physical_status){ .response = {QMI_RESULT_FAILURE_V01, QMI_ERR_INTERNAL_V01} };
	struct hk_qmi_projection p = { .decode = uim_slots_project, .output = out };
	return radio_project_query(qmi, 0x47, 0, NULL, NULL, &p, timeout_ms);
}

/* Nonblocking WDA/WDS queries for the serial MHI poll owner. These are the
 * same public wire projections as the synchronous entry points. 0x004d only
 * changes the requesting WDS client's IP-family preference; no packet handle
 * is returned, and no packet-service or persistent profile is started. */
static int
family_pref_project(const void *packet, size_t size, void *output)
{
	struct radio_tlv_state state;
	const uint8_t *tlvs,*v;
	size_t left,n;
	uint8_t type;
	uint16_t result,error;
	int rc=radio_begin(packet,size,0x4d,&state,&result,&error,&tlvs,&left);
	if (rc) return rc;
	while (left) {
		rc=radio_next(&tlvs,&left,&type,&v,&n);
		if (rc) return rc;
		if (type==2) {
			if (state.result_seen) return -EBADMSG;
			rc=radio_common_tlv(v,n,&result,&error);
			if (rc) return rc;
			state.result_seen=true;
		} else if (type<0x10) return -EBADMSG;
	}
	if (!state.result_seen) return -EBADMSG;
	*(struct qmi_response_type_v01 *)output=(struct qmi_response_type_v01){result,error};
	return (int)size;
}

static int
family_nas_project(const void *packet, size_t size, void *output)
{
	struct nas_serving_wire wire={0};
	int rc=radio_validate_serving(packet,size);
	if (rc) return rc;
	rc=hk_qmi_decode(packet,size,serving_response,&wire);
	if (rc<0) return rc;
	struct hk_qmi_nas_serving_status *out=output;
	out->response=wire.response;
	out->valid=!wire.response.result && wire.valid;
	if (out->valid) {
		out->registration=wire.fields[0];out->cs_attach=wire.fields[1];out->ps_attach=wire.fields[2];
        const uint8_t *cursor=(const uint8_t *)packet+QMI_HEADER_SIZE,*value;
        size_t remaining=size-QMI_HEADER_SIZE,bytes;uint8_t type;unsigned tags=0;
        while(remaining) {
            int err=radio_next(&cursor,&remaining,&type,&value,&bytes);
            if(err)return err;
            if(type==0x10){tags++;out->roaming_valid=(tags==1 && bytes==1);if(out->roaming_valid)out->roaming=value[0];}
        }
	}
	return rc;
}

int
hk_qmi_family_begin(struct qmi_handle *qmi,struct hk_qmi_family_op *op,
    enum hk_qmi_family_query which,uint8_t family)
{
	static const struct qmi_elem_info pref_ei[]={
		{QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,1,0,NULL},{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
	};
	static const uint32_t endpoint[2]={3,4};
	uint16_t msg;
	size_t max=0;
	const struct qmi_elem_info *ei=NULL;
	const void *request=NULL;
	int rc;
	if (!qmi || !op || (which==HK_FQ_SET_FAMILY ? family!=4 && family!=6 : family!=0)) return -EINVAL;
	memset(op,0,sizeof(*op));
	switch (which) {
	case HK_FQ_FORMAT:
		msg=HK_QMI_WDA_GET_DATA_FORMAT;ei=wda_pcie_request;max=11;request=endpoint;
		op->projection=(struct hk_qmi_projection){wda_project,&op->value.format};
		rc=hk_qmi_txn_init_projected(qmi,&op->txn,&op->projection);break;
	case HK_FQ_STATUS:
		msg=HK_QMI_WDS_GET_PACKET_SERVICE_STATUS;
		rc=qmi_txn_init_checked(qmi,&op->txn,hk_qmi_wds_packet_service_status_resp_ei,
		    &op->value.status,radio_validate_packet_status);break;
	case HK_FQ_SET_FAMILY:
		msg=0x4d;ei=pref_ei;max=4;request=&family;
		op->projection=(struct hk_qmi_projection){family_pref_project,&op->value.family_ack};
		rc=hk_qmi_txn_init_projected(qmi,&op->txn,&op->projection);break;
	case HK_FQ_SERVING:
		msg=HK_QMI_NAS_GET_SERVING_SYSTEM;
		op->projection=(struct hk_qmi_projection){family_nas_project,&op->value.serving};
		rc=hk_qmi_txn_init_projected(qmi,&op->txn,&op->projection);break;
	default:return -EINVAL;
	}
	if (rc<0) return rc;
	rc=qmi_send_request(qmi,NULL,&op->txn,msg,max,ei,request);
	if (rc) qmi_txn_cancel(&op->txn);
	return rc;
}

/* UIM Event Registration and Get ATR. Retain the returned mask or ATR length
 * only, never the ATR data or any identifier. */
static int
uim_obs_scalar_project(const void *packet,size_t length,void *output)
{
	struct hk_uim_obs_scalar *reply=output;
	struct hk_uim_obs_scalar r={.message=reply->message};
	struct radio_tlv_state state;
	uint16_t result,error;
	const uint8_t *tlvs,*value;
	size_t remaining,bytes;
	uint8_t type;
	int rc=radio_begin(packet,length,r.message,&state,&result,&error,&tlvs,&remaining);
	if (rc) return rc;
	while (remaining) {
		rc=radio_next(&tlvs,&remaining,&type,&value,&bytes);
		if (rc) return rc;
		if (type==2) {
			if (state.result_seen) return -EBADMSG;
			rc=radio_common_tlv(value,bytes,&result,&error);
			if (rc) return rc;
			state.result_seen=true;
		} else if (type==0x10) {
			if (r.present) return -EBADMSG;
			r.present=1;
			if (r.message==0x2e) {
				if (bytes!=4) return -EBADMSG;
				r.mask=slot_u32(value);
			} else if (r.message==0x41) {
				if (!bytes || value[0]!=bytes-1) return -EBADMSG;
				r.atr_length=value[0];
			} else return -EINVAL;
		} else if (type<0x10) return -EBADMSG;
	}
	if (!state.result_seen) return -EBADMSG;
	if (result) {r.present=0;r.mask=0;r.atr_length=0;}
	r.result=result;r.error=error;*reply=r;
	return (int)length;
}

/* Normal UIM extended and WMS ready wire layouts from Qualcomm's published
 * GobiConnectionMgmt 2013-07-31 API structs. No AID, ATR or PIN data is kept. */
static int uim_obs_ext_slots(const uint8_t *v, size_t n, struct hk_qmi_uim_status *card,
    const uint8_t *validity, unsigned validity_count)
{
	uint16_t indices[2] = {0xffff,0xffff};
	if (!n || 1u+2u*v[0] > n) return -EBADMSG;
	unsigned count = *v++;
	n--;
	for (unsigned i=0; i<count; i++, v+=2, n-=2) {
		if (i<2) indices[i] = (uint16_t)v[0]<<8 | v[1];
	}
	if (!n || 1u+2u*v[0] > n) return -EBADMSG;
	size_t cdma = 1u+2u*v[0];
	return uim_card_array(v+cdma,n-cdma,card,indices,validity,validity_count);
}

static int uim_obs_ext_project(const void *packet,size_t length,void *output)
{
	struct hk_uim_obs_extended r={0};
	struct radio_tlv_state state;
	const uint8_t *tlvs,*value,*validity=NULL,*extended=NULL;
	size_t extended_size=0;
	size_t remain,bytes;
	uint8_t type;
	uint16_t result,error;
	int rc=radio_begin(packet,length,0x2f,&state,&result,&error,&tlvs,&remain);
	if (rc) return rc;
	while (remain) {
		rc=radio_next(&tlvs,&remain,&type,&value,&bytes);
		if (rc) return rc;
		if (type==2) {
			if (state.result_seen) return -EBADMSG;
			rc=radio_common_tlv(value,bytes,&result,&error);
			if (rc) return rc;
			state.result_seen=true;
		} else if (type==0x12) {
			if (r.validity_present || !bytes || value[0]!=bytes-1) return -EBADMSG;
			r.validity_present=1;
			r.valid_count=value[0];
			validity=value+1;
			for (unsigned i=0; i<r.valid_count && i<2; i++) r.valid[i]=value[i+1];
		} else if (type==0x13) {
			if (r.extended) return -EBADMSG;
			extended=value;
			extended_size=bytes;
			r.extended=1;
		} else if (type<0x10) {
			return -EBADMSG;
		}
	}
	if (!state.result_seen) return -EBADMSG;
	if (r.extended) {
		r.card.validity_present=r.validity_present;
		r.card.validity_count=r.valid_count;
		rc=uim_obs_ext_slots(extended,extended_size,&r.card,validity,r.valid_count);
		if (rc) return rc;
	}
	if (result || error) memset(&r,0,sizeof(r));
	else if(r.extended) r.card.valid=1;
	r.result=result;
	r.error=error;
	*(struct hk_uim_obs_extended *)output=r;
	return (int)length;
}


int
hk_qmi_uim_obs_begin(struct qmi_handle *q,struct hk_uim_obs_op *op,enum hk_uim_obs_query query,uint32_t arg)
{
	static const struct qmi_elem_info u32_one[]={
		{QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,1,0,NULL},{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}};
	static const struct qmi_elem_info u8_one[]={
		{QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,1,0,NULL},{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}};
	static const struct qmi_elem_info extended[]={
		{QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,0x10,0,NULL},{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}};
	const struct qmi_elem_info *ei=NULL;
	const void *request=NULL;
	uint8_t byte=(uint8_t)arg;
	uint16_t msg;
	size_t size=0;
	if (!q || !op) return -EINVAL;
	memset(op,0,sizeof(*op));
	switch (query) {
	case HK_UO_REGISTER:
		if (arg!=0 && arg!=0x15) return -EINVAL;
		msg=0x2e;ei=u32_one;request=&arg;size=7;
		op->value.scalar.message=msg;
		op->projection=(struct hk_qmi_projection){uim_obs_scalar_project,&op->value.scalar};
		break;
	case HK_UO_ATR:
		if (arg!=1 && arg!=2) return -EINVAL;
		msg=0x41;ei=u8_one;request=&byte;size=4;
		op->value.scalar.message=msg;
		op->projection=(struct hk_qmi_projection){uim_obs_scalar_project,&op->value.scalar};
		break;
	case HK_UO_SLOTS:
		if (arg) return -EINVAL;
		msg=0x47;op->projection=(struct hk_qmi_projection){uim_slots_project,&op->value.slots};
		break;
	case HK_UO_EXTENDED:
		if (arg) return -EINVAL;
		msg=0x2f;byte=1;ei=extended;request=&byte;size=4;
		op->projection=(struct hk_qmi_projection){uim_obs_ext_project,&op->value.ext};
		break;
	case HK_UO_LEGACY:
		if (arg) return -EINVAL;
		msg=0x2f;op->projection=(struct hk_qmi_projection){uim_status_project,&op->value.card};break;
	case HK_UO_DMS:
		if (arg) return -EINVAL;
		msg=HK_QMI_DMS_GET_OPERATING_MODE;break;
	default:return -EINVAL;
	}
	int rc=query==HK_UO_DMS?qmi_txn_init_checked(q,&op->txn,hk_qmi_dms_operating_mode_resp_ei,&op->value.dms,radio_validate_mode):
	    hk_qmi_txn_init_projected(q,&op->txn,&op->projection);
	if (rc<0) return rc;
	rc=qmi_send_request(q,NULL,&op->txn,msg,size,ei,request);
	if (rc) qmi_txn_cancel(&op->txn);
	return rc;
}

/* Appended to hk_qmi_radio.c after the #276 read-only projections. The receive
 * dispatcher already verifies source, message and transaction. Only the
 * chosen application selector may survive the callback, for the next send. */
static void ua_wipe(void *buf, size_t n)
{
    volatile uint8_t *p = buf;
    while (n--) *p++ = 0;
}
void hk_uim_act_forget(struct hk_uim_act_once *a)
{
    if (!a) return;
    ua_wipe(a->selector, sizeof(a->selector));
    a->selector_length = 0;
    a->owner = NULL;
    a->choice = HK_UA_NONE;
    /* The one-shot latch and modem ACK receipt survive cleanup. */
}
static bool ua_physical_ok(const struct hk_uim_physical_status *p)
{
    return p && p->valid && p->info_valid && !p->response.result && !p->response.error &&
        p->count == 2 && p->info_count == 2 && p->slots[0].card == 2 &&
        p->slots[0].activity == 1 && p->slots[0].logical_slot == 1 &&
        p->slots[0].protocol == 2 && p->slots[0].atr_present &&
        p->slots[1].card == 1 && !p->slots[1].atr_present && p->slots[1].logical_slot != 1;
}
/* The enclosing decoder has validated the length of every record. */
static const uint8_t *ua_unique_usim(const uint8_t *app, unsigned count, unsigned *index)
{
    const uint8_t *selected = NULL;
    for (unsigned i=0; i<count; i++) {
        if (app[0] == 2) {
            if (selected) return NULL;
            selected = app;
            *index = i;
        }
        app += 14u + app[6];
    }
    return selected;
}
static int ua_choose_project(const void *data, size_t length, void *out)
{
    struct hk_uim_act_once *a = out;
    struct hk_qmi_uim_status s = {0};
    ua_wipe(a->selector, sizeof(a->selector));
    a->selector_length = 0;
    a->choice = HK_UA_NONE;
    /* Validate the entire TLV stream, including bounds of every application,
     * duplicate tags, app count, GW references and separate validity data. */
    int rc = uim_status_project(data, length, &s);
    if (rc < 0) return rc;
    if (!s.valid || s.response.result || s.response.error || s.slots != 2 ||
        s.logical[0].card_state != 1 || s.logical[1].card_state == 1 ||
        (s.validity_present && s.logical[0].validity != HK_UIM_VALIDITY_VALID))
        return rc;
    const uint8_t *p = (const uint8_t *)data + QMI_HEADER_SIZE;
    size_t remaining = length - QMI_HEADER_SIZE;
    const uint8_t *card = NULL, *extended = NULL;
    while (remaining) {
        uint8_t type;
        const uint8_t *value;
        size_t bytes;
        int err = radio_next(&p, &remaining, &type, &value, &bytes);
        if (err) return err;
        if (type == 0x10) card = value;
        if (type == 0x13) extended = value;
    }
    if (!card) return rc;
    /* 4 GW/1X references, card count, slot-1 fixed record. Slot 1 is first. */
    unsigned index = 0;
    const uint8_t *selected = ua_unique_usim(card+15, s.logical[0].app_count, &index);
    if (!selected) return rc;
    /* Some servers send both card-status formats. Check both complete arrays
     * before using a selector. Ignore unrelated opaque PIN/personalization data. */
    if (extended) {
        struct hk_uim_obs_extended ext = {0};
        int err = uim_obs_ext_project(data, length, &ext);
        if (err < 0) return err;
        if (!ext.extended || ext.card.slots != 2 || ext.card.logical[0].card_state != 1 ||
            ext.card.logical[1].card_state == 1 ||
            ext.card.logical[0].app_count != s.logical[0].app_count) return rc;
        for (unsigned i=0; i<2; i++)
            if (ext.card.gw[i].status != s.gw[i].status || ext.card.gw[i].index != s.gw[i].index) return rc;
        const uint8_t *cdma = extended + 1 + 2u*extended[0];
        const uint8_t *records = cdma + 1 + 2u*cdma[0];
        unsigned ei=0;
        const uint8_t *other=ua_unique_usim(records+7,ext.card.logical[0].app_count,&ei);
        if (!other || ei != index || other[1] != selected[1] || other[6] != selected[6] ||
            selected[6] > sizeof(a->selector) || memcmp(other+7, selected+7, selected[6])) return rc;
    }
    if (s.gw[0].status == HK_UIM_GW_MAPPED && s.gw[0].index == index) {
        a->app_index=(uint8_t)index; a->choice = HK_UA_ALREADY;
        return rc;
    }
    /* Any existing or invalid primary reference is outside this authorization.
     * Do not move an application already used by the secondary session. */
    if (s.gw[0].status != HK_UIM_GW_UNASSIGNED ||
        (s.gw[1].status != HK_UIM_GW_UNASSIGNED &&
         (s.gw[1].status != HK_UIM_GW_MAPPED || s.gw[1].index == index)))
        return rc;
    /* Supported for this trial: DETECTED or READY. PIN/PUK/personalization,
     * illegal and unknown states are observable only. ISO application IDs
     * contain a five-byte RID and at most eleven further bytes. */
    unsigned n = selected[6];
    if ((selected[1] != 1 && selected[1] != 7) || n < 5 || n > sizeof(a->selector)) return rc;
    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < n; i++) { any |= selected[7+i]; all &= selected[7+i]; }
    if (!any || all == 0xff) return rc; /* all-filler is not a usable selector */
    memcpy(a->selector, selected + 7, n);
    a->selector_length = (uint8_t)n;
    a->app_index=(uint8_t)index; a->choice = HK_UA_ELIGIBLE;
    return rc;
}
int hk_uim_act_choose(struct qmi_handle *q, struct hk_uim_act_once *a,
    const struct hk_uim_act_stamp *stamp, const struct hk_uim_physical_status *slots,
    uint8_t direct_atr_length)
{
    if (!a || !q || !stamp || !stamp->epoch || !stamp->generation || !stamp->local || !stamp->node || !stamp->port)
        return -EINVAL;
    if (a->attempted || a->owner) return -EBUSY;
    hk_uim_act_forget(a);
    if (!ua_physical_ok(slots) || !direct_atr_length) return -EINVAL;
    a->owner = q;
    a->stamp = *stamp;
    a->projection = (struct hk_qmi_projection){ua_choose_project, a};
    int rc = hk_qmi_txn_init_projected(q, &a->txn, &a->projection);
    if (rc >= 0) {
        rc = qmi_send_request(q, NULL, &a->txn, 0x2f, 0, NULL, NULL);
        if (rc) qmi_txn_cancel(&a->txn);
    }
    if (rc) hk_uim_act_forget(a);
    return rc;
}
struct ua_app { uint8_t slot; uint32_t length; uint8_t id[16]; };
struct ua_request { uint8_t session[2]; struct ua_app app; };
static const struct qmi_elem_info ua_app_ei[] = {
    {QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,0,offsetof(struct ua_app,slot),NULL},
    {QMI_DATA_LEN,1,1,NO_ARRAY,0,offsetof(struct ua_app,length),NULL},
    {QMI_UNSIGNED_1_BYTE,16,1,VAR_LEN_ARRAY,0,offsetof(struct ua_app,id),NULL},
    {QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
};
static const struct qmi_elem_info ua_request_ei[] = {
    {QMI_UNSIGNED_1_BYTE,2,1,STATIC_ARRAY,1,offsetof(struct ua_request,session),NULL},
    {QMI_STRUCT,1,sizeof(struct ua_app),NO_ARRAY,0x10,offsetof(struct ua_request,app),ua_app_ei},
    {QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
};
static int ua_ack(const void *data, size_t length, void *out)
{
    struct hk_uim_act_once *a = out;
    struct hk_uim_obs_scalar ack = {.message=0x38};
    int rc = uim_obs_scalar_project(data, length, &ack);
    if (rc > 0) { a->ack_seen=1; a->result=ack.result; a->error=ack.error; }
    return rc;
}
int hk_uim_act_send(struct qmi_handle *q, struct hk_uim_act_once *a, const struct hk_uim_act_stamp *stamp)
{
    if (!a) return -EINVAL;
    if (!q || !stamp || q != a->owner || memcmp(stamp, &a->stamp, sizeof(*stamp)) ||
        a->attempted || a->choice != HK_UA_ELIGIBLE || !a->txn.done || a->txn.result <= 0 ||
        a->txn.peer_node != stamp->node || a->txn.peer_port != stamp->port ||
        a->selector_length < 5 || a->selector_length > sizeof(a->selector)) {
        hk_uim_act_forget(a);
        return -EINVAL;
    }
    struct ua_request req = {.session={0,1},.app={.slot=1,.length=a->selector_length}};
    memcpy(req.app.id, a->selector, req.app.length);
    hk_uim_act_forget(a);
    /* Safe both after normal poll/collect and after a synchronously completed
     * reply; never reuse a transaction object that is still on the list. */
    qmi_txn_cancel(&a->txn);
    struct sockaddr_qrtr peer = {.sq_node=stamp->node, .sq_port=stamp->port};
    a->projection = (struct hk_qmi_projection){ua_ack, a};
    int rc = hk_qmi_txn_init_projected(q, &a->txn, &a->projection);
    if (rc >= 0) {
        /* Burn before transport submission. Doorbell failure/timeout can be
         * ambiguous, so neither errors nor negative ACKs can rearm the latch. */
        a->attempted = 1;
        rc = qmi_send_request(q, &peer, &a->txn, 0x38, 10u + req.app.length,
            ua_request_ei, &req);
        if (rc) qmi_txn_cancel(&a->txn);
        else a->submitted = 1;
    }
    ua_wipe(&req, sizeof(req));
    return rc;
}

/* Append to frozen #277 hk_qmi_radio.c. Reuses its checked TS projections
 * and DMS Get Operating Mode. This encoder has just one possible target. */
static bool online_stamp_ok(const struct hk_uim_act_stamp *stamp)
{
    return stamp && stamp->generation && stamp->epoch && stamp->node &&
        stamp->port && stamp->local;
}
static bool online_proven(const struct qmi_txn *tx, const struct qmi_handle *owner,
    const struct hk_uim_act_stamp *stamp, uint16_t message)
{
    return tx && owner && online_stamp_ok(stamp) && tx->qmi == owner && tx->bound &&
        tx->done && tx->result > 0 && tx->peer_node == stamp->node &&
        tx->peer_port == stamp->port && tx->message == message;
}
static bool online_mode_ok(const struct hk_qmi_dms_operating_mode_resp *m)
{
    return !m->response.result && !m->response.error && m->mode_valid &&
        (m->mode == 1 || m->mode == 3) &&
        !(m->offline_reason_valid && m->offline_reason) &&
        !(m->hardware_restricted_valid && m->hardware_restricted);
}
bool hk_online_usim_primary_ready(const struct hk_qmi_uim_status *card)
{
    if (!card || !card->valid || card->response.result || card->response.error ||
        card->logical_count<1 || card->slots<1) return false;
    const struct hk_uim_logical_slot *slot=&card->logical[0];
    const struct hk_uim_gw_binding *gw=&card->gw[0];
    return slot->card_state==1 && slot->validity!=HK_UIM_VALIDITY_INVALID &&
        slot->validity!=HK_UIM_VALIDITY_OTHER && slot->ready_gw_apps &&
        (slot->usim_states&(1u<<7)) && gw->status==HK_UIM_GW_MAPPED &&
        (gw->index>>8)==0 && (gw->index&255u)<slot->app_count &&
        gw->app_type==2 && gw->app_state==7;
}
int hk_online_ts_begin(struct qmi_handle *q, struct hk_online_ts_op *op,
    const struct hk_uim_act_stamp *stamp, uint32_t seq, uint64_t now_ms)
{
    if (!q || !op || !online_stamp_ok(stamp) || !now_ms || op->owner) return -EINVAL;
    struct ts_read_request req = { .name="modem_tsens", .current=1, .sequence=seq };
    op->owner=q;
    op->stamp=*stamp;
    op->seq=seq;
    op->start_ms=now_ms;
    op->projection = seq ?
        (struct hk_qmi_projection){ts_ack_project,&op->ack} :
        (struct hk_qmi_projection){ts_list_project,&op->list};
    int rc=hk_qmi_txn_init_projected(q,&op->txn,&op->projection);
    if (rc<0) return rc;
    rc=qmi_send_request(q,NULL,&op->txn,seq?0x21:0x20,seq?62:0,
        seq?ts_current_ei:NULL,seq?&req:NULL);
    if (rc) qmi_txn_cancel(&op->txn);
    return rc;
}
void hk_online_ts_ind(struct hk_online_ts_op *op,struct qmi_handle *q,
    const struct hk_uim_act_stamp *live,const struct sockaddr_qrtr *peer,
    const struct hk_qmi_ts_report *r,uint64_t now_ms)
{
    if (!op || !q || op->owner!=q || !live || !peer || !r || !op->seq ||
        memcmp(live,&op->stamp,sizeof(*live)) || peer->sq_node!=live->node ||
        peer->sq_port!=live->port || now_ms<op->start_ms ||
        !r->name_valid || memcmp(r->name,"modem_tsens",sizeof("modem_tsens")) ||
        !r->kind_valid || r->kind || !r->sequence_valid || r->sequence!=op->seq ||
        !r->temperature_valid) return;
    int mc;
    if (hk_qmi_ts_millicelsius(r->temperature_bits,&mc)) { op->conflict=true; return; }
    if (op->sampled) { if (mc!=op->sample_mc) op->conflict=true; return; }
    op->sampled=true;
    op->sample_mc=mc;
    op->sample_ms=now_ms;
}
int hk_online_arm(struct qmi_handle *q,struct hk_online_once *once,
    const struct hk_online_scope *scope,const struct hk_uim_obs_op *first,uint64_t now_ms)
{
    if (!once || once->attempted || once->arm_checked) return -EINVAL;
    once->arm_checked=true;
    if (!scope || !first || !now_ms ||
        !online_stamp_ok(&scope->uim) || !online_stamp_ok(&scope->ts) ||
        !online_proven(&first->txn,q,&scope->dms,HK_QMI_DMS_GET_OPERATING_MODE) ||
        !online_mode_ok(&first->value.dms)) return -EINVAL;
    once->owner=q;
    once->scope=*scope;
    once->original=first->value.dms;
    once->first_txid=first->txn.id;
    once->armed_ms=now_ms;
    once->armed=true;
    return 0;
}
static int online_ack_project(const void *data,size_t length,void *out)
{
    struct hk_online_once *once=out;
    struct radio_tlv_state state;
    const uint8_t *tlvs,*value;
    size_t remaining,bytes;
    uint8_t type;
    uint16_t result,error;
    int rc=radio_begin(data,length,0x002e,&state,&result,&error,&tlvs,&remaining);
    if (rc) return rc;
    while (remaining) {
        rc=radio_next(&tlvs,&remaining,&type,&value,&bytes);
        if (rc) return rc;
        if (type==2) {
            if (state.result_seen) return -EBADMSG;
            rc=radio_common_tlv(value,bytes,&result,&error);
            if (rc) return rc;
            state.result_seen=true;
        } else if (type<0x10) return -EBADMSG;
    }
    if (!state.result_seen) return -EBADMSG;
    once->result=result;
    once->error=error;
    once->ack_seen=true;
    return (int)length;
}
static int dms_checked_target(struct qmi_handle *q,struct hk_online_once *once,
    const struct hk_online_scope *live,const struct hk_uim_obs_op *second,
    const struct hk_online_ts_op *list,const struct hk_online_ts_op *current,
    bool fresh_ready_primary,bool wds_disconnected,uint64_t now_ms,uint8_t target)
{
    static const struct qmi_elem_info target_ei[]={
        {QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,1,0,NULL},
        {QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
    };
    if (!once || !once->armed || once->attempted) return -EINVAL;
    if (target==1 && once->original.mode!=3) return -EINVAL;
    once->armed=false; /* A failed final gate ends this arm. Owner ends trial. */
    if (!live || !second || !list || !current || !q || !fresh_ready_primary || !wds_disconnected ||
        once->owner!=q || memcmp(live,&once->scope,sizeof(*live)) ||
        now_ms<once->armed_ms || now_ms-once->armed_ms>3000 ||
        !online_proven(&second->txn,q,&live->dms,HK_QMI_DMS_GET_OPERATING_MODE) ||
        second->txn.id==once->first_txid || !online_mode_ok(&second->value.dms) ||
        once->original.mode!=second->value.dms.mode ||
        once->original.offline_reason_valid!=second->value.dms.offline_reason_valid ||
        once->original.hardware_restricted_valid!=second->value.dms.hardware_restricted_valid ||
        !list->owner || list->owner!=current->owner || list->seq || !current->seq ||
        memcmp(&list->stamp,&live->ts,sizeof(live->ts)) ||
        memcmp(&current->stamp,&live->ts,sizeof(live->ts)) ||
        !online_proven(&list->txn,list->owner,&live->ts,0x20) ||
        !online_proven(&current->txn,current->owner,&live->ts,0x21) ||
        list->list.response.result || list->list.response.error || !list->list.valid ||
        !list->list.has_modem_tsens || current->ack.result || current->ack.error ||
        !current->sampled || current->conflict || now_ms<current->sample_ms ||
        now_ms-current->sample_ms>3000 || now_ms<current->start_ms ||
        now_ms-current->start_ms>3000 || current->sample_mc<0 || current->sample_mc>=45000)
        return -EINVAL;
    once->projection=(struct hk_qmi_projection){online_ack_project,once};
    int rc=hk_qmi_txn_init_projected(q,&once->txn,&once->projection);
    if (rc<0) return rc;
    once->wire_target=target;
    once->attempted=true;
    rc=qmi_send_request(q,NULL,&once->txn,0x002e,4,target_ei,&target);
    once->submitted=(rc==0);
    if (rc) qmi_txn_cancel(&once->txn);
    return rc;
}

int hk_online_send(struct qmi_handle *q,struct hk_online_once *once,
    const struct hk_online_scope *scope,const struct hk_uim_obs_op *second,
    const struct hk_online_ts_op *list,const struct hk_online_ts_op *temp,bool ready,bool disconnected,uint64_t now)
{
    return dms_checked_target(q,once,scope,second,list,temp,ready,disconnected,now,0);
}

int hk_donor_low_send(struct qmi_handle *q,struct hk_online_once *once,
    const struct hk_online_scope *scope,const struct hk_uim_obs_op *second,
    const struct hk_online_ts_op *list,const struct hk_online_ts_op *temp,bool ready,bool disconnected,uint64_t now)
{
    return dms_checked_target(q,once,scope,second,list,temp,ready,disconnected,now,1);
}

bool hk_donor_low_ack(struct qmi_handle *q,const struct hk_online_once *op,const struct hk_online_scope *scope)
{
    return op && scope && op->wire_target==1 && op->owner==q && op->attempted && op->submitted &&
        op->ack_seen && !memcmp(&op->scope,scope,sizeof(*scope)) && online_proven(&op->txn,q,&scope->dms,0x002e) &&
        ((!op->result && !op->error) || (op->result==1 && op->error));
}

bool hk_donor_low_read(struct qmi_handle *q,const struct hk_online_once *op,const struct hk_online_scope *scope,
    const struct hk_uim_obs_op *r)
{
    if (!r || !hk_donor_low_ack(q,op,scope) || !online_proven(&r->txn,q,&scope->dms,HK_QMI_DMS_GET_OPERATING_MODE) ||
        r->txn.id==op->txn.id || r->txn.id==op->first_txid) return false;
    const struct hk_qmi_dms_operating_mode_resp *v=&r->value.dms;
    return !v->response.result && !v->response.error && v->mode_valid &&
        (v->mode==0 || v->mode==1 || v->mode==3) &&
        !(v->offline_reason_valid && v->offline_reason) && !(v->hardware_restricted_valid && v->hardware_restricted);
}

int pn_get_sw_sub0(uint16_t txn,void *wire,size_t n)
{
    struct native_request {uint32_t type,sub;} req={1,0};
    static const struct qmi_elem_info ei[]={
        {QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,0x01,offsetof(struct native_request,type),NULL},
        {QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,0x11,offsetof(struct native_request,sub),NULL},
        {QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
    };
    if (!txn || !wire) return -EINVAL;
    return hk_qmi_encode(QMI_REQUEST,0x22,txn,ei,&req,wire,n);
}
static void pn_decode_one(struct pn_reply *r,uint16_t txn,uint32_t token,const void *wire,size_t n)
{
    const uint8_t *p=wire,*v;
    if (!p || n<7 || !r || radio_u16(p+3)!=0x22) return;
    bool ack=p[0]==QMI_RESPONSE;
    if (ack ? (r->ack || radio_u16(p+1)!=txn) : (r->ind || p[0]!=QMI_INDICATION)) return;
    if (n>8192 || radio_u16(p+5)!=n-7) {r->bad_length++;return;}
    p+=7;n-=7;
    uint16_t result=0,error=0;bool common=false,active=false,pending=false,token_seen=false;
    uint8_t active_len=0,pending_len=0;
    while(n) {
        uint8_t tag;size_t len;
        if (radio_next(&p,&n,&tag,&v,&len)) goto bad;
        if (tag==0x10) {
            if (!token) {r->unexpected_token++;return;}
            if (token_seen || len!=4) goto bad;
            if (slot_u32(v)!=token) {r->unexpected_token++;return;}
            token_seen=true;continue;
        }
        if (tag==(ack?2:1)) {
            if (common) goto bad;
            if (ack) {if (radio_common_tlv(v,len,&result,&error)) goto bad;}
            else {if(len!=2)goto bad;error=radio_u16(v);}
            common=true;
        } else if (!ack && (tag==0x11 || tag==0x12)) {
            bool *present=tag==0x11?&active:&pending;
            if (*present || !len || v[0]>124 || len!=1u+v[0]) goto bad;
            *present=true;
            if(tag==0x11)active_len=v[0];else pending_len=v[0];
        } else if (tag<0x10) goto bad;
    }
    if (!ack && token && !token_seen) {r->unexpected_token++;return;}
    if(!common)goto bad;
    if(ack) {r->ack=true;r->result=result;r->error=error;}
    else {r->ind=true;r->ind_first=!r->ack;r->ind_error=error;r->active=active;r->pending=pending;
          r->active_length=active_len;r->pending_length=pending_len;}
    return;
bad:
    if(ack)r->bad_ack++;else r->bad_ind++;
}
/* Decode every candidate independently. Two valid indications are ambiguous
 * even if all reportable fields agree; no comparison or retention of ID
 * bytes is necessary. The native SW listener remains open to its deadline. */
void pn_decode(struct pn_reply *r,uint16_t txn,uint32_t token,const void *wire,size_t n)
{
    struct pn_reply v={0};
    pn_decode_one(&v,txn,token,wire,n);
    r->bad_ack+=v.bad_ack;r->bad_ind+=v.bad_ind;r->bad_length+=v.bad_length;
    r->unexpected_token+=v.unexpected_token;
    if(v.ack) {
        ++r->ack_candidates;
        if(r->ack && (r->result!=v.result || r->error!=v.error)) ++r->ack_conflicts;
        if(!r->ack) {r->ack=true;r->result=v.result;r->error=v.error;}
    }
    if(v.ind) {
        ++r->ind_candidates;
        if(r->ind && (r->ind_error!=v.ind_error || r->active!=v.active || r->pending!=v.pending ||
            r->active_length!=v.active_length || r->pending_length!=v.pending_length)) ++r->ind_summary_conflicts;
        if(!r->ind) {
            r->ind=true;r->ind_first=!r->ack;r->ind_error=v.ind_error;
            r->active=v.active;r->pending=v.pending;r->active_length=v.active_length;r->pending_length=v.pending_length;
        }
    }
}
int pcs_hw_request(uint16_t txn,uint32_t token,void *wire,size_t size)
{
    struct hw_read { uint32_t type,token; } req={0,token};
    static const struct qmi_elem_info ei[]={
        {QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,1,offsetof(struct hw_read,type),NULL},
        {QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,0x10,offsetof(struct hw_read,token),NULL},
        {QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}
    };
    if (!txn || !token || !wire) return -EINVAL;
    return hk_qmi_encode(QMI_REQUEST,0x22,txn,ei,&req,wire,size);
}

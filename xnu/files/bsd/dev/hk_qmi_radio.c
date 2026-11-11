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
	if (result) memset(&r,0,sizeof(r));
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

/* Host tests for the read-only DMS/NAS/WDS radio queries. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/hk_qmi_radio.h>
#include <sys/hk_qrtr.h>

#define SERVER_NODE 7u
#define SERVER_PORT 0x4001u

static unsigned checks, failures;

static void
check(bool condition, const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

enum reply_kind {
	REPLY_NONE,
	REPLY_MODEL_OK,
	REPLY_MODEL_FAILURE,
	REPLY_MODE_OK,
	REPLY_SIGNAL_OK,
	REPLY_SERVING_OK,
	REPLY_SERVING_EMPTY,
	REPLY_SERVING_BAD_COUNT,
	REPLY_CARD_READY,
	REPLY_CARD_EMPTY,
	REPLY_CARD_TRUNCATED,
	REPLY_CARD_ERRORS,
	REPLY_WDA_PAIRED,
	REPLY_WDA_PARTIAL,
	REPLY_WDA_DUPLICATE,
	REPLY_WDA_FAILURE,
	REPLY_TS_LIST,
	REPLY_TS_BAD_LIST,
	REPLY_TS_ACK,
	REPLY_PHYSICAL_SLOTS,
	REPLY_PHYSICAL_BAD_LENGTH,
	REPLY_PACKET_OK,
	REPLY_MISSING_RESULT,
	REPLY_MISSING_PRIMARY,
	REPLY_DUPLICATE_PRIMARY,
	REPLY_SHORT_PRIMARY,
	REPLY_MODEL_MAX,
	REPLY_EMBEDDED_NUL,
	REPLY_BAD_UNKNOWN,
	REPLY_TRAILING,
	REPLY_WRONG_TYPE,
	REPLY_INDICATION,
	REPLY_WRONG_ENDPOINT,
	REPLY_WRONG_MESSAGE,
};

struct hk_qrtr_port {
	hk_qrtr_receive_t receive;
	hk_qrtr_server_t server;
	void *context;
	bool open;
	unsigned sent;
	uint32_t last_node, last_port;
	uint8_t last[256];
	size_t last_length;
	int send_error;
	enum reply_kind reply;
};

static struct hk_qrtr_port router;

static struct hk_qrtr_port *
open_port(hk_qrtr_receive_t receive, hk_qrtr_server_t server, void *context)
{
	memset(&router, 0, sizeof(router));
	router.receive = receive;
	router.server = server;
	router.context = context;
	router.open = true;
	return &router;
}

static void
append_tlv(uint8_t *reply, size_t *at, uint8_t type, const void *value, size_t length)
{
	reply[(*at)++] = type;
	reply[(*at)++] = (uint8_t)length;
	reply[(*at)++] = (uint8_t)(length >> 8);
	memcpy(reply + *at, value, length);
	*at += length;
}

static void
build_reply(const uint8_t *request, size_t request_length, uint8_t *reply, size_t *reply_length,
	enum reply_kind kind)
{
	uint8_t result[4] = { 0, 0, 0, 0 };
	uint8_t value[128];
	size_t at = QMI_HEADER_SIZE;
	uint8_t type = QMI_RESPONSE;

	(void)request_length;
	memset(reply, 0, 256);
	if (kind == REPLY_WRONG_TYPE || kind == REPLY_INDICATION) {
		type = kind == REPLY_INDICATION ? QMI_INDICATION : QMI_REQUEST;
	}
	if (kind == REPLY_MISSING_RESULT) {
		memset(value, 0, 1);
		append_tlv(reply, &at, 0x01, value, 1);
	} else {
		if (kind == REPLY_MODEL_FAILURE || kind == REPLY_WDA_FAILURE) {
			result[0] = 1;
			result[2] = QMI_ERR_NOT_SUPPORTED_V01;
		}
		append_tlv(reply, &at, 0x02, result, sizeof(result));
		if (kind == REPLY_MODEL_OK || kind == REPLY_DUPLICATE_PRIMARY || kind == REPLY_SHORT_PRIMARY ||
		    kind == REPLY_MODEL_MAX || kind == REPLY_EMBEDDED_NUL) {
			memcpy(value, "SDX55", 5);
			if (kind == REPLY_SHORT_PRIMARY) {
				memset(value + 5, 'x', 123);
			}
			if (kind == REPLY_MODEL_MAX) {
				memset(value + 5, 'x', 122);
			}
			if (kind == REPLY_EMBEDDED_NUL) {
				value[0] = 'A';
				value[1] = 0;
				value[2] = 'B';
			}
			append_tlv(reply, &at, 0x01, value,
			    kind == REPLY_SHORT_PRIMARY ? 128 : kind == REPLY_MODEL_MAX ? 127 : kind == REPLY_EMBEDDED_NUL ? 3 : 5);
			if (kind == REPLY_DUPLICATE_PRIMARY) {
				append_tlv(reply, &at, 0x01, value, 5);
			}
		} else if (kind == REPLY_MODE_OK) {
			value[0] = 2;
			append_tlv(reply, &at, 0x01, value, 1);
			value[0] = 0x34;
			value[1] = 0x12;
			append_tlv(reply, &at, 0x10, value, 2);
			value[0] = 1;
			append_tlv(reply, &at, 0x11, value, 1);
			value[0] = 0xaa;
			append_tlv(reply, &at, 0x90, value, 1);
		} else if (kind == REPLY_SIGNAL_OK) {
			value[0] = (uint8_t)-83;
			value[1] = 8;
			append_tlv(reply, &at, 0x01, value, 2);
		} else if (kind == REPLY_SERVING_EMPTY) {
			const uint8_t serving[] = { 0, 0, 0, 0, 0 };
			append_tlv(reply, &at, 0x01, serving, sizeof(serving));
		} else if (kind == REPLY_SERVING_OK || kind == REPLY_SERVING_BAD_COUNT) {
			uint8_t serving[] = { 1, 1, 2, 0, 3, 8, 12, 0xff };
			if (kind == REPLY_SERVING_BAD_COUNT) {
				serving[4]++;
			}
			append_tlv(reply, &at, 0x01, serving, sizeof(serving));
		} else if (kind == REPLY_CARD_READY || kind == REPLY_CARD_TRUNCATED) {
			uint8_t card[31] = {0};
			card[8] = 1;   /* one slot */
			card[9] = 1;   /* card present */
			card[14] = 1;  /* one application */
			card[16] = 7;  /* ready */
			card[21] = kind == REPLY_CARD_READY ? 2 : 65;
			append_tlv(reply, &at, 0x10, card, sizeof(card));
		} else if (kind == REPLY_CARD_ERRORS) {
			/* Three error states and an absent slot that carries a stale error byte. */
			uint8_t card[33] = {0};
			card[8] = 4;
			card[9] = card[15] = card[21] = 2;
			card[13] = 1; card[19] = 3; card[25] = 200; card[31] = 5;
			append_tlv(reply, &at, 0x10, card, sizeof(card));
		} else if (kind == REPLY_CARD_EMPTY) {
			const uint8_t card[9] = {0};
			append_tlv(reply, &at, 0x10, card, sizeof(card));
		} else if (kind == REPLY_WDA_PAIRED || kind == REPLY_WDA_PARTIAL ||
		    kind == REPLY_WDA_DUPLICATE || kind == REPLY_WDA_FAILURE) {
			const uint8_t qos = 0, raw_ip[4] = {2}, no_aggregation[4] = {0}, qmap5[4] = {9};
			append_tlv(reply, &at, 0x10, &qos, 1);
			append_tlv(reply, &at, 0x11, raw_ip, sizeof(raw_ip));
			if (kind != REPLY_WDA_PARTIAL) {
				append_tlv(reply, &at, 0x12, no_aggregation, sizeof(no_aggregation));
				append_tlv(reply, &at, 0x13, qmap5, sizeof(qmap5));
			}
			if (kind == REPLY_WDA_DUPLICATE) {
				append_tlv(reply, &at, 0x11, raw_ip, sizeof(raw_ip));
			}
		} else if (kind == REPLY_PHYSICAL_SLOTS || kind == REPLY_PHYSICAL_BAD_LENGTH) {
			uint8_t physical[] = {2, 2,0,0,0, 1,0,0,0, 1,2, 0xaa,0xbb,
			                         1,0,0,0, 0,0,0,0, 0,0};
			uint8_t info[] = {2, 2,0,0,0, 0,2, 0xaa,0xbb,0, 0,0,0,0, 0,0,0};
			if (kind == REPLY_PHYSICAL_BAD_LENGTH) physical[10] = 240;
			append_tlv(reply,&at,0x11,info,sizeof(info));
			append_tlv(reply,&at,0x10,physical,sizeof(physical));
		} else if (kind == REPLY_TS_LIST || kind == REPLY_TS_BAD_LIST) {
			uint8_t list[] = {2, 2, 'p','a', 11, 'm','o','d','e','m','_','t','s','e','n','s'};
			if (kind == REPLY_TS_BAD_LIST) list[4] = 31;
			append_tlv(reply, &at, 0x10, list, sizeof(list));
		} else if (kind == REPLY_PACKET_OK) {
			value[0] = 2;
			append_tlv(reply, &at, 0x01, value, 1);
		}
	}
	if (kind == REPLY_BAD_UNKNOWN) {
		reply[at++] = 0x90;
		reply[at++] = 4;
		reply[at++] = 0;
		reply[at++] = 0xaa;
	}
	if (kind == REPLY_TRAILING) {
		/* Leave the declared body length one byte short; qmi_receive accepts the
		 * datagram, while the radio validator rejects its trailing byte. */
		reply[at++] = 0xee;
	}
	reply[0] = type;
	reply[1] = request[1];
	reply[2] = request[2];
	reply[3] = request[3];
	reply[4] = request[4];
	reply[5] = (uint8_t)(at - QMI_HEADER_SIZE);
	reply[6] = (uint8_t)((at - QMI_HEADER_SIZE) >> 8);
	if (kind == REPLY_TRAILING) {
		reply[5]--;
	}
	*reply_length = at;
}

static void
deliver_from(uint32_t node, uint32_t port, const uint8_t *reply, size_t length)
{
	router.receive(router.context, node, port, reply, length);
}

static void
peer(struct hk_qrtr_port *port, const uint8_t *request, size_t length)
{
	uint8_t reply[256];
	size_t reply_length;

	if (port->reply == REPLY_NONE) {
		return;
	}
	build_reply(request, length, reply, &reply_length, port->reply);
	if (port->reply == REPLY_WRONG_ENDPOINT) {
		deliver_from(SERVER_NODE + 1, SERVER_PORT, reply, reply_length);
	} else if (port->reply == REPLY_WRONG_MESSAGE) {
		reply[3] ^= 1;
		deliver_from(SERVER_NODE, SERVER_PORT, reply, reply_length);
	} else {
		deliver_from(SERVER_NODE, SERVER_PORT, reply, reply_length);
	}
}

static int
send_datagram(struct hk_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (port->send_error != 0) {
		return port->send_error;
	}
	port->sent++;
	port->last_node = node;
	port->last_port = to_port;
	port->last_length = length;
	memcpy(port->last, data, length);
	peer(port, data, length);
	return 0;
}

static int
lookup_service(struct hk_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	(void)port;
	(void)service;
	(void)version;
	(void)instance;
	return 0;
}

static void
close_port(struct hk_qrtr_port *port)
{
	port->open = false;
}

static const struct hk_qrtr_ops fake_ops = { open_port, send_datagram, lookup_service, close_port };

static int
new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	return hk_qmi_connect((struct socket *)qmi->sock, service->node, service->port);
}

static void
del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	(void)qmi;
	(void)service;
}

static const struct qmi_ops qmi_ops = { .new_server = new_server, .del_server = del_server };

static void
announce(bool arrived)
{
	router.server(router.context, arrived, HK_QMI_DMS_SERVICE, 1, 0, SERVER_NODE, SERVER_PORT);
}

struct pending_call {
	struct qmi_handle *qmi;
	int result;
};

static void *
pending_model(void *arg)
{
	struct pending_call *call = arg;
	struct hk_qmi_dms_model_resp response;

	call->result = hk_qmi_dms_get_model(call->qmi, &response, 1000);
	return NULL;
}

static void
test_radio(void)
{
	struct qmi_handle qmi;
	struct hk_qmi_dms_model_resp model;
	struct hk_qmi_dms_operating_mode_resp mode;
	struct hk_qmi_nas_signal_strength_req signal_request = { .request_mask = 0 };
	struct hk_qmi_nas_signal_strength_resp signal;
	struct hk_qmi_nas_serving_status serving;
	struct hk_qmi_uim_status card;
	struct hk_qmi_wda_format format;
	struct hk_qmi_ts_list ts;
	struct qmi_response_type_v01 ack;
	struct hk_qmi_wds_packet_service_status_resp packet;
	struct pending_call pending;
	pthread_t thread;

	check(qmi_handle_init_transport(&qmi, 1024, &qmi_ops, NULL, &fake_ops) == 0, "radio handle initializes");
	announce(true);

	router.reply = REPLY_MODEL_OK;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) > 0 && model.response.result == 0 && model.model_valid &&
	    strcmp(model.model, "SDX55") == 0 && router.last_length == QMI_HEADER_SIZE && router.last[3] == 0x22 &&
	    router.last[4] == 0, "DMS Get Model decodes the required string and sends an empty request");

	router.reply = REPLY_MODE_OK;
	check(hk_qmi_dms_get_operating_mode(&qmi, &mode, 100) > 0 && mode.mode_valid && mode.mode == 2 &&
	    mode.offline_reason_valid && mode.offline_reason == 0x1234 && mode.hardware_restricted_valid &&
	    mode.hardware_restricted == 1 && router.last_length == QMI_HEADER_SIZE && router.last[3] == 0x2d &&
	    router.last[4] == 0, "DMS operating mode keeps known optional TLVs");

	router.reply = REPLY_SIGNAL_OK;
	check(hk_qmi_nas_get_signal_strength(&qmi, &signal_request, &signal, 100) > 0 && signal.signal_strength_valid &&
	    signal.signal_strength == -83 && signal.radio_interface == 8 && router.last_length == 12 &&
	    router.last[7] == 0x10 && router.last[8] == 2 && router.last[9] == 0 && router.last[10] == 0 && router.last[11] == 0,
	    "NAS signal query sends its exact mask TLV and decodes RSSI");

	router.reply = REPLY_SERVING_OK;
	check(hk_qmi_nas_get_serving_system(&qmi, &serving, 100) > 0 && serving.valid &&
	    serving.registration == 1 && serving.cs_attach == 1 && serving.ps_attach == 2 &&
	    serving.known_interfaces == ((1u << 8) | (1u << 12)) && serving.unknown_interfaces == 1 &&
	    router.last_length == QMI_HEADER_SIZE && router.last[3] == 0x24 && router.last[4] == 0,
	    "NAS serving system sends an empty request and preserves attachment and RAT values");
	router.reply = REPLY_SERVING_EMPTY;
	check(hk_qmi_nas_get_serving_system(&qmi, &serving, 100) > 0 && serving.valid &&
	    !serving.interface_count && !serving.registration, "a valid empty radio list is reported");
	router.reply = REPLY_SERVING_BAD_COUNT;
	check(hk_qmi_nas_get_serving_system(&qmi, &serving, 100) < 0 && !serving.valid,
	    "a truncated interface list cannot publish a serving state");

	router.reply = REPLY_CARD_READY;
	check(hk_qmi_uim_get_card_status(&qmi, &card, 100) > 0 && card.valid && card.slots == 1 &&
	    card.card_state[1] == 1 && card.application_state[7] == 1 &&
	    router.last_length == QMI_HEADER_SIZE && router.last[3] == 0x2f && router.last[4] == 0,
	    "UIM card and application counts use an empty Get Card Status request");
	router.reply = REPLY_CARD_EMPTY;
	check(hk_qmi_uim_get_card_status(&qmi, &card, 100) > 0 && card.valid && card.slots == 0,
	    "zero slots with a complete status TLV is a measured value");
	router.reply = REPLY_CARD_TRUNCATED;
	check(hk_qmi_uim_get_card_status(&qmi, &card, 100) < 0 && !card.valid,
	    "a truncated card application record cannot produce a readiness outcome");
	router.reply = REPLY_CARD_ERRORS;
	check(hk_qmi_uim_get_card_status(&qmi, &card, 100) > 0 && card.valid && card.card_state[2] == 3 &&
	    card.card_state[0] == 1 && card.error_mask == 10 && card.unrecognized_errors == 1,
	    "UIM retains power-down and no-ATR reasons only on error slots; later codes remain unknown");

	router.reply = REPLY_PACKET_OK;
	check(hk_qmi_wds_get_packet_service_status(&qmi, &packet, 100) > 0 && packet.connection_status_valid &&
	    packet.connection_status == 2 && router.last_length == QMI_HEADER_SIZE && router.last[3] == 0x22 &&
	    router.last[4] == 0, "WDS packet status decodes");

	const uint8_t pcie_request[] = { 0x10, 8, 0, 3, 0, 0, 0, 4, 0, 0, 0 };
	router.reply = REPLY_WDA_PAIRED;
	check(hk_qmi_wda_get_pcie_format(&qmi, &format, 100) > 0 && format.present == 15 &&
	    format.value[HK_WDA_LINK] == 2 && format.value[HK_WDA_UPLINK] == 0 && format.value[HK_WDA_DOWNLINK] == 9 &&
	    router.last_length == QMI_HEADER_SIZE + sizeof(pcie_request) && router.last[3] == 0x21 && router.last[4] == 0 &&
	    !memcmp(router.last + QMI_HEADER_SIZE, pcie_request, sizeof(pcie_request)),
	    "WDA requests PCIe endpoint 4 and preserves different uplink and downlink formats");
	router.reply = REPLY_WDA_PARTIAL;
	check(hk_qmi_wda_get_pcie_format(&qmi, &format, 100) > 0 && format.present == 3,
	    "omitted aggregation fields stay unknown");
	router.reply = REPLY_WDA_DUPLICATE;
	check(hk_qmi_wda_get_pcie_format(&qmi, &format, 100) < 0 && !format.present,
	    "ambiguous format cannot be published");
	router.reply = REPLY_WDA_FAILURE;
	check(hk_qmi_wda_get_pcie_format(&qmi, &format, 100) > 0 && format.response.result == 1 && !format.present,
	    "failed WDA request does not report a negotiated format");

	struct hk_uim_physical_status physical;
	router.reply = REPLY_PHYSICAL_SLOTS;
	check(hk_qmi_uim_get_physical_slots(&qmi,&physical,100)>0 && !physical.response.result &&
	    physical.valid && physical.info_valid && physical.count==2 && physical.info_count==2 &&
	    physical.slots[0].card==2 && physical.slots[0].logical_slot==1 && physical.slots[0].atr_present &&
	    physical.slots[0].protocol==2 && physical.slots[1].card==1 && !physical.slots[1].atr_present &&
	    router.last_length==QMI_HEADER_SIZE && router.last[3]==0x47,
	    "physical SIM statuses and ATR availability decode independently of array order; no opaque ID output");
	router.reply = REPLY_PHYSICAL_BAD_LENGTH;
	check(hk_qmi_uim_get_physical_slots(&qmi,&physical,100)<0 && !physical.valid,
	    "an invalid opaque field length invalidates the whole slot result");
	router.reply = REPLY_TS_LIST;
	check(hk_qmi_ts_list(&qmi,&ts,100)>0 && ts.valid && ts.count==2 && ts.has_modem_tsens &&
	    router.last_length==QMI_HEADER_SIZE && router.last[3]==0x20, "TS enumerates native modem_tsens");
	router.reply = REPLY_TS_BAD_LIST;
	check(hk_qmi_ts_list(&qmi,&ts,100)<0 && !ts.valid, "truncated thermal name rejected");
	router.reply = REPLY_TS_ACK;
	const uint8_t thermal_req[] = {1,12,0,11,'m','o','d','e','m','_','t','s','e','n','s',
	    2,1,0,1,0x12,4,0,7,0,0,0};
	check(hk_qmi_ts_current(&qmi,7,&ack,100)>0 && !ack.result &&
	    router.last_length==QMI_HEADER_SIZE+sizeof(thermal_req) && router.last[3]==0x21 &&
	    !memcmp(router.last+QMI_HEADER_SIZE,thermal_req,sizeof(thermal_req)),
	    "temperature read requests a current observation, matched sequence, no thresholds");
	uint8_t thermal_ind[] = {4,1,0,0x22,0,36,0,1,12,0,11,'m','o','d','e','m','_','t','s','e','n','s',
	    2,4,0,0,0,0,0,0x10,4,0,0,0,0x22,0x42,0x11,4,0,7,0,0,0};
	struct hk_qmi_ts_report report = {0};
	int millic = 0;
	check(hk_qmi_decode(thermal_ind,sizeof(thermal_ind),hk_qmi_ts_report_ei,&report)>0 &&
	    report.name_valid && !strcmp(report.name,"modem_tsens") && report.kind_valid && !report.kind &&
	    report.sequence_valid && report.sequence==7 && report.temperature_valid &&
	    !hk_qmi_ts_millicelsius(report.temperature_bits,&millic) && millic==40500,
	    "source sensor, current report, matching sequence and 40.5 C decode");
	check(!hk_qmi_ts_millicelsius(0xc0a00000u,&millic) && millic==-5000 &&
	    hk_qmi_ts_millicelsius(0x7fc00000u,&millic)<0, "signed temperature and nonfinite rejection");

	router.reply = REPLY_MODEL_FAILURE;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) > 0 && model.response.result == QMI_RESULT_FAILURE_V01 &&
	    model.response.error == QMI_ERR_NOT_SUPPORTED_V01, "modem failure remains visible in the response");

	router.reply = REPLY_MISSING_RESULT;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0 && model.response.result == QMI_RESULT_FAILURE_V01,
	    "missing common result is rejected");
	router.reply = REPLY_MISSING_PRIMARY;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "missing required model is rejected");
	router.reply = REPLY_DUPLICATE_PRIMARY;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "duplicate known TLV is rejected");
	router.reply = REPLY_SHORT_PRIMARY;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "short known TLV is rejected");
	router.reply = REPLY_MODEL_MAX;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) > 0 && model.model_valid && strlen(model.model) == 127,
	    "maximum model string that fits its C buffer is accepted");
	router.reply = REPLY_EMBEDDED_NUL;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "embedded NUL in model is rejected");
	router.reply = REPLY_BAD_UNKNOWN;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "truncated unknown optional TLV is rejected");
	router.reply = REPLY_TRAILING;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "trailing bytes are rejected");
	router.reply = REPLY_WRONG_TYPE;
	check(hk_qmi_dms_get_model(&qmi, &model, 100) < 0, "wrong QMI type is rejected");
	router.reply = REPLY_INDICATION;
	check(hk_qmi_wds_get_packet_service_status(&qmi, &packet, 100) < 0, "WDS indication is not mistaken for a response");
	router.reply = REPLY_WRONG_ENDPOINT;
	check(hk_qmi_dms_get_model(&qmi, &model, 20) == -ETIMEDOUT, "response from another endpoint cannot complete a transaction");
	router.reply = REPLY_WRONG_MESSAGE;
	check(hk_qmi_dms_get_model(&qmi, &model, 20) == -ETIMEDOUT, "response for another message cannot complete a transaction");

	router.reply = REPLY_NONE;
	check(hk_qmi_dms_get_model(&qmi, &model, 20) == -ETIMEDOUT, "radio query timeout is reported");
	/* Service removal completes an in-flight query and prevents a new implicit send. */
	pending.qmi = &qmi;
	pending.result = 0;
	pthread_create(&thread, NULL, pending_model, &pending);
	usleep(20000);
	announce(false);
	pthread_join(thread, NULL);
	check(pending.result == -ENOTCONN, "service removal wakes a pending radio query");
	router.reply = REPLY_MODEL_OK;
	check(hk_qmi_dms_get_model(&qmi, &model, 20) == -ENOTCONN, "disconnected service blocks implicit sends");
	announce(true);
	router.send_error = EHOSTUNREACH;
	check(hk_qmi_dms_get_model(&qmi, &model, 20) == -EHOSTUNREACH, "transport send error is returned");
	router.send_error = 0;

	qmi_handle_release(&qmi);
}

int
main(void)
{
	test_radio();
	printf("%s radio QMI tests (%u checks)\n", failures == 0 ? "ok" : "FAIL", checks);
	return failures != 0;
}

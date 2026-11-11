#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hk_qmi.h>
#include <sys/hk_qmi_loc.h>
#include <sys/hk_qrtr.h>

struct hk_qrtr_port {
	unsigned marker;
};

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static struct {
	struct hk_qrtr_port port;
	hk_qrtr_receive_t receive;
	hk_qrtr_server_t server;
	void *context;
	unsigned lookups, sends, closes;
	uint32_t node, service_port;
} fake;

static struct hk_qrtr_port *
fake_open(hk_qrtr_receive_t receive, hk_qrtr_server_t server, void *context)
{
	fake.receive = receive;
	fake.server = server;
	fake.context = context;
	fake.port.marker = 0x4c4f43;
	return &fake.port;
}

static int
fake_send(struct hk_qrtr_port *port, uint32_t node, uint32_t to_port,
	const void *data, size_t length)
{
	const uint8_t *wire = data;
	struct hk_qmi_loc_start_req request = { 0 };
	struct hk_qmi_loc_start_resp response = { { 0, 0 }, 1, 7 };
	struct hk_qmi_loc_inject_utc_req utc = { 0 };
	struct hk_qmi_loc_register_events_req events = { 0 };
	struct hk_qmi_loc_stop_req stop = { 0 };
	struct hk_qmi_loc_common_resp common = { { 0, 0 } };
	uint8_t reply[128];
	uint16_t message;
	int n;

	CHECK(port == &fake.port);
	CHECK(node == fake.node && to_port == fake.service_port);
	CHECK(length >= QMI_HEADER_SIZE && wire[0] == QMI_REQUEST);
	message = (uint16_t)(wire[3] | (uint16_t)wire[4] << 8);
	if (message == HK_QMI_LOC_START) {
		CHECK(hk_qmi_decode(data, length, hk_qmi_loc_start_req_ei, &request) == (int)length);
		CHECK(request.session_id == 1 && request.recurrence == 1 && request.intermediate == 1 && request.interval_ms == 1000);
		n = hk_qmi_encode(QMI_RESPONSE, message,
		    (uint16_t)(wire[1] | (uint16_t)wire[2] << 8), hk_qmi_loc_start_resp_ei,
		    &response, reply, sizeof(reply));
	} else if (message == HK_QMI_LOC_INJECT_UTC) {
		CHECK(hk_qmi_decode(data, length, hk_qmi_loc_inject_utc_req_ei, &utc) == (int)length);
		CHECK(utc.utc_ms == 1700000000123ull && utc.uncertainty_ms == 5000);
		n = hk_qmi_encode(QMI_RESPONSE, message,
		    (uint16_t)(wire[1] | (uint16_t)wire[2] << 8), hk_qmi_loc_common_resp_ei,
		    &common, reply, sizeof(reply));
	} else if (message == HK_QMI_LOC_REGISTER_EVENTS) {
		CHECK(hk_qmi_decode(data, length, hk_qmi_loc_register_events_req_ei, &events) == (int)length);
		CHECK(events.event_mask == 0x83 && events.client_type_valid && events.client_type == 1);
		n = hk_qmi_encode(QMI_RESPONSE, message,
		    (uint16_t)(wire[1] | (uint16_t)wire[2] << 8), hk_qmi_loc_common_resp_ei,
		    &common, reply, sizeof(reply));
	} else if (message == HK_QMI_LOC_STOP) {
		CHECK(hk_qmi_decode(data, length, hk_qmi_loc_stop_req_ei, &stop) == (int)length);
		CHECK(stop.session_id == 1);
		n = hk_qmi_encode(QMI_RESPONSE, message,
		    (uint16_t)(wire[1] | (uint16_t)wire[2] << 8), hk_qmi_loc_common_resp_ei,
		    &common, reply, sizeof(reply));
	} else {
		CHECK(0 && "unexpected LOC message");
		return EINVAL;
	}
	CHECK(n > QMI_HEADER_SIZE);
	fake.sends++;
	fake.receive(fake.context, fake.node, fake.service_port, reply, (size_t)n);
	return 0;
}

static int
fake_lookup(struct hk_qrtr_port *port, uint32_t service, uint32_t version,
	uint32_t instance)
{
	CHECK(port == &fake.port && service == HK_QMI_LOC_SERVICE && version == 2 && instance == 0);
	fake.lookups++;
	fake.server(fake.context, true, service, version, instance, fake.node, fake.service_port);
	return 0;
}

static void
fake_close(struct hk_qrtr_port *port)
{
	CHECK(port == &fake.port);
	fake.closes++;
}

static const struct hk_qrtr_ops fake_ops = {
	.open = fake_open,
	.send = fake_send,
	.lookup = fake_lookup,
	.close = fake_close,
};

static int
on_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	return hk_qmi_connect(qmi->sock, service->node, service->port);
}

static const struct qmi_ops qmi_ops = {
	.new_server = on_server,
};

int
main(void)
{
	struct qmi_handle qmi = { 0 };
	struct hk_qmi_loc_inject_utc_req utc = { 1700000000123ull, 5000 };
	struct hk_qmi_loc_register_events_req events = { 0x83, 1, 1 };
	struct hk_qmi_loc_stop_req stop = { 1 };
	struct hk_qmi_loc_common_resp common = { { 0, 0 } };
	struct hk_qmi_loc_start_req request = { 1, 1, 1, 1000 };
	struct hk_qmi_loc_start_resp response = { 0 };
	static const uint8_t position[] = {
		QMI_INDICATION, 1, 0, HK_QMI_LOC_POSITION_REPORT & 0xff,
		HK_QMI_LOC_POSITION_REPORT >> 8, 20, 0,
		1, 4, 0, 0, 0, 0, 0,
		0x12, 4, 0, 0, 0, 0x20, 0x41,
		0x2c, 3, 0, 1, 0x34, 0x12,
	};
	struct hk_qmi_loc_position_report_ind indication = { 0 };
	int result;

	fake.node = 3;
	fake.service_port = 83;
	CHECK(qmi_handle_init_transport(&qmi, 8192, &qmi_ops, NULL, &fake_ops) == 0);
	CHECK(qmi_add_lookup(&qmi, HK_QMI_LOC_SERVICE, 2, 0) == 0);
	CHECK(fake.lookups == 1);
	result = hk_qmi_loc_start(&qmi, &request, &response, 1000);
	CHECK(result > QMI_HEADER_SIZE && response.response.result == QMI_RESULT_SUCCESS_V01 &&
	    response.response.error == QMI_ERR_NONE_V01 && response.session_id_valid && response.session_id == 7);
	CHECK(fake.sends == 1);
	result = hk_qmi_loc_inject_utc(&qmi, &utc, &common, 1000);
	CHECK(result > QMI_HEADER_SIZE && common.response.result == QMI_RESULT_SUCCESS_V01 - 0 && common.response.error == QMI_ERR_NONE_V01);
	result = hk_qmi_loc_register_events(&qmi, &events, &common, 1000);
	CHECK(result > QMI_HEADER_SIZE && common.response.result == QMI_RESULT_SUCCESS_V01 && common.response.error == QMI_ERR_NONE_V01);
	result = hk_qmi_loc_stop(&qmi, &stop, &common, 1000);
	CHECK(result > QMI_HEADER_SIZE && common.response.result == QMI_RESULT_SUCCESS_V01 && common.response.error == QMI_ERR_NONE_V01);
	CHECK(fake.sends == 4);
	CHECK(hk_qmi_loc_start(NULL, &request, &response, 1) == -EINVAL);
	CHECK(hk_qmi_loc_start(&qmi, NULL, &response, 1) == -EINVAL);
	CHECK(hk_qmi_loc_start(&qmi, &request, NULL, 1) == -EINVAL);
	CHECK(hk_qmi_loc_inject_utc(NULL, &utc, &common, 1) == -EINVAL);
	CHECK(hk_qmi_loc_register_events(&qmi, NULL, &common, 1) == -EINVAL);
	CHECK(hk_qmi_loc_stop(&qmi, &stop, NULL, 1) == -EINVAL);
	result = hk_qmi_loc_decode_position_report(position, sizeof(position), &indication);
	CHECK(result == (int)sizeof(position) && indication.status_valid && indication.status == 0 &&
	    indication.uncertainty_valid && indication.uncertainty_bits == 0x41200000 &&
	    indication.satellites_used_valid && indication.satellites_used_len == 1 &&
	    indication.satellites_used[0] == 0x1234);
	CHECK(hk_qmi_loc_decode_position_report(NULL, sizeof(position), &indication) == -EINVAL);
	qmi_handle_release(&qmi);
	CHECK(fake.closes == 1);

	printf("qmi-loc-client-test: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hk_qmi.h>
#include <sys/hk_qmi_loc.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void check_bytes(const uint8_t *got, int n, const uint8_t *want, size_t m)
{ CHECK(n == (int)m && memcmp(got, want, m) == 0); }

int main(void)
{
	uint8_t b[512]; int n;
	struct hk_qmi_loc_inject_utc_req utc = { 1700000000123ull, 5000 };
	static const uint8_t utc_wire[] = { 0, 0x34, 0x12, 0x38, 0, 18, 0,
		1, 8, 0, 0x7b, 0x68, 0xe5, 0xcf, 0x8b, 1, 0, 0, 2, 4, 0, 0x88, 0x13, 0, 0 };
	n = hk_qmi_encode(QMI_REQUEST, HK_QMI_LOC_INJECT_UTC, 0x1234, hk_qmi_loc_inject_utc_req_ei, &utc, b, sizeof(b));
	check_bytes(b, n, utc_wire, sizeof(utc_wire));
	struct hk_qmi_loc_inject_utc_req utc2 = { 0 };
	CHECK(hk_qmi_decode(b, n, hk_qmi_loc_inject_utc_req_ei, &utc2) == n && utc2.utc_ms == utc.utc_ms && utc2.uncertainty_ms == 5000);

	struct hk_qmi_loc_register_events_req reg = { 0x83, 1, 1 };
	n = hk_qmi_encode(QMI_REQUEST, HK_QMI_LOC_REGISTER_EVENTS, 1, hk_qmi_loc_register_events_req_ei, &reg, b, sizeof(b));
	CHECK(n > 7 && b[7] == 1 && b[18] == 0x11);
	struct hk_qmi_loc_register_events_req reg2 = { 0 };
	CHECK(hk_qmi_decode(b, n, hk_qmi_loc_register_events_req_ei, &reg2) == n && reg2.event_mask == 0x83 && reg2.client_type_valid && reg2.client_type == 1);
	reg.client_type_valid = 0;
	n = hk_qmi_encode(QMI_REQUEST, HK_QMI_LOC_REGISTER_EVENTS, 2, hk_qmi_loc_register_events_req_ei, &reg, b, sizeof(b));
	CHECK(n == 18 && b[7] == 1 && b[18 - 1] == 0);

	struct hk_qmi_loc_start_req start = { 1, 1, 1, 1000 };
	n = hk_qmi_encode(QMI_REQUEST, HK_QMI_LOC_START, 3, hk_qmi_loc_start_req_ei, &start, b, sizeof(b));
	CHECK(n == 32 && b[7] == 1 && b[11] == 0x10 && b[18] == 0x12 && b[25] == 0x13);
	struct hk_qmi_loc_start_req start2 = { 0 };
	CHECK(hk_qmi_decode(b, n, hk_qmi_loc_start_req_ei, &start2) == n && start2.session_id == 1 && start2.interval_ms == 1000);

	static const uint8_t response[] = { 2, 9, 0, 0x22, 0, 11, 0, 2, 4, 0, 0, 0, 0, 0, 1, 1, 0, 1 };
	struct hk_qmi_loc_start_resp sr = { 0 };
	CHECK(hk_qmi_decode(response, sizeof(response), hk_qmi_loc_start_resp_ei, &sr) == (int)sizeof(response));
	CHECK(sr.response.result == 0 && sr.response.error == 0 && sr.session_id_valid && sr.session_id == 1);

	static const uint8_t pos[] = { 4, 1, 0, 0x24, 0, 20, 0, 1, 4, 0, 0, 0, 0, 0, 0x12, 4, 0, 0, 0, 0x20, 0x41, 0x2c, 3, 0, 1, 0x34, 0x12 };
	struct hk_qmi_loc_position_report_ind pi = { 0 };
	CHECK(hk_qmi_decode(pos, sizeof(pos), hk_qmi_loc_position_report_ind_ei, &pi) == (int)sizeof(pos));
	CHECK(pi.status_valid && pi.status == 0 && pi.uncertainty_valid && pi.uncertainty_bits == 0x41200000 && pi.satellites_used_valid && pi.satellites_used_len == 1 && pi.satellites_used[0] == 0x1234);

	uint8_t sv[7 + 3 + 1 + 28 * 2] = { 4, 2, 0, 0x25, 0, 0, 0, 0x10, 57, 0, 2 };
	for (unsigned i = 0; i < 28; i++) sv[11 + i] = (uint8_t)i;
	for (unsigned i = 0; i < 28; i++) sv[39 + i] = (uint8_t)(0xa0 + i);
	sv[5] = (uint8_t)((sizeof(sv) - 7) & 0xff); sv[6] = (uint8_t)((sizeof(sv) - 7) >> 8);
	struct hk_qmi_loc_sv_info_ind si = { 0 };
	CHECK(hk_qmi_decode(sv, sizeof(sv), hk_qmi_loc_sv_info_ind_ei, &si) == (int)sizeof(sv));
	CHECK(si.entries_len == 2 && si.entries[0].bytes[0] == 0 && si.entries[0].bytes[27] == 27 && si.entries[1].bytes[0] == 0xa0);

	printf("qmi-loc-test: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}

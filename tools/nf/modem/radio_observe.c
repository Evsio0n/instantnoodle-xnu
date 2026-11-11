/* Public counters only. The logger is a background job in the RAM test root. */
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "mdm_radio_status.h"

static void
report_radio(const struct hk_radio_snapshot *current)
{
    static const char *const names[HK_RADIO_COUNT] = { "mode", "signal", "packet" };
    printf("radio-observe: phase=%u", current->phase);
    for (unsigned i = 0; i < HK_RADIO_COUNT; ++i) {
        const struct hk_radio_field *field = &current->fields[i];
        printf(" %s.state=%u", names[i], field->state);
        if (field->state == HK_RADIO_VALUE)
            printf(" value=%d detail=%d", field->value, field->detail);
        else if (field->error)
            printf(" error=%d", field->error);
    }
    puts("");
    fflush(stdout);
}

static void
report_sv_counts(void)
{
    char text[512] = {0};
    size_t size = sizeof(text) - 1;
    unsigned wire, absent, empty, bad, no_cn0;
    if (sysctlbyname("kern.hackios.modem_gnss", text, &size, NULL, 0) != 0 ||
        size >= sizeof(text) || strncmp(text, "enabled=1 ", 10) != 0)
        return;
    const char *counts = strstr(text, " sv_wire=");
    if (counts && sscanf(counts, " sv_wire=%u sv_absent=%u sv_empty=%u sv_bad=%u sv_no_cn0=%u",
                         &wire, &absent, &empty, &bad, &no_cn0) == 5) {
        printf("gnss-observe: wire=%u absent=%u empty=%u malformed=%u no_cn0=%u\n",
               wire, absent, empty, bad, no_cn0);
        fflush(stdout);
    }
}

int
main(void)
{
    pid_t worker = fork();
    if (worker < 0) return 2;
    if (worker > 0) return 0;
    struct hk_radio_snapshot last = {0};
    unsigned emitted = 0;
    for (unsigned poll = 0; poll < 120; ++poll) {
        struct hk_radio_snapshot current = {0};
        size_t size = sizeof(current);
        if (sysctlbyname("kern.hackios.modem_radio_status", &current, &size, NULL, 0) == 0 &&
            size == sizeof(current) && current.version == HK_RADIO_VERSION && current.size == size &&
            (!emitted || current.phase != last.phase ||
             memcmp(current.fields, last.fields, sizeof(current.fields)) != 0)) {
            report_radio(&current);
            last = current;
            ++emitted;
        }
        if (poll % 15 == 0) report_sv_counts();
        sleep(2);
    }
    printf("radio-observe: finished snapshots=%u\n", emitted);
    return emitted ? 0 : 1;
}

/* Wi-Fi-OFF sleep control. One foreground helper enforces prerequisite exits.
 * A failed lock/display check MUST NOT fall through to IOPMSleepSystem.
 * No network, storage writes, screenshots, retries or peripheral controls here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef CONTROL_SLEEP
#define CONTROL_SLEEP "5"
#endif
#ifndef CONTROL_OBSERVE
#define CONTROL_OBSERVE "0"
#endif
static int run(char *const args[]) {
    pid_t pid = fork();
    int status;
    if (pid < 0) return 1;
    if (!pid) { execv(args[0], args); _exit(127); }
    if (waitpid(pid, &status, 0) != pid) return 1;
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}
static int panel_is(int expected) {
    int value = -1;
    size_t size = sizeof(value);
    if (sysctlbyname("kern.hackios.display_on", &value, &size, NULL, 0) != 0 ||
        size != sizeof(value) || value != expected) {
        printf("wifi-sleep-control: FAIL panel state unavailable or unexpected\n");
        return 0;
    }
    return 1;
}
#define STEP(...) do { char *args[] = { __VA_ARGS__, NULL }; \
    if (run(args)) { printf("wifi-sleep-control: FAIL step=%d; no further actions\n", ++step); fflush(stdout); return 1; } ++step; } while (0)
int main(void) {
    int step = 0;
    setbuf(stdout, NULL);
    printf("wifi-sleep-control: BEGIN wifi-OFF control, one " CONTROL_SLEEP "-second sleep\n");
    STEP("/hackios/bin/rwtouch", "frames", "20", "90", "idle", "60", "20", "mark", "wifi-sleep-control-ready");
    STEP("/hackios/bin/power_test", "status");
    if (!panel_is(1)) return 1;
    STEP("/hackios/bin/rwtouch", "button", "home", "wait", "2", "button", "lock");
    STEP("/hackios/bin/power_test", "waitdisplay", "off", "5");
    if (!panel_is(0)) return 1;
    STEP("/hackios/bin/power_test", "sleep", CONTROL_SLEEP, "1");
    STEP("/hackios/bin/power_test", "waitdisplay", "on", "8");
    if (!panel_is(1)) return 1;
    STEP("/hackios/bin/power_test", "status");
    printf("wifi-sleep-control: PASS timer and display checks; not Wi-Fi resume proof\n");
    if (atoi(CONTROL_OBSERVE) > 0) {
        printf("wifi-sleep-control: observation window " CONTROL_OBSERVE " seconds; owner may check screen and touch now\n");
        char *args[] = { "/hackios/bin/rwtouch", "wait", CONTROL_OBSERVE, NULL };
        if (run(args)) return 1;
    }
    return 0;
}

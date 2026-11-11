#!/usr/bin/env python3
"""Compile the CURRENT control.c on host, replacing execv only; no device effects.
Requires numeric-sysctl controls to be reviewed/mockable separately if added.
"""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
build = Path(tempfile.mkdtemp(prefix='control-host-', dir=HERE))
source = r'''
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>
static int logfd, fail_at, signal_at, panel_calls, panel_bad_at, panel_mode;
static int mock_sysctlbyname(const char *name, void *value, size_t *size, void *newp, size_t newlen) {
 assert(strcmp(name, "kern.hackios.display_on") == 0);
 int index = ++panel_calls;
 *(int *)value = index == 2 ? 0 : 1;
 if (index == panel_bad_at) {
  if (panel_mode == 0) return -1;
  if (panel_mode == 1) *size = 1;
  if (panel_mode == 2) *(int *)value = -1;
  if (panel_mode == 3) *(int *)value = index == 2 ? 1 : 0;
 }
 return 0;
}
static int mock_execv(const char *path, char *const args[]) {
 off_t index = lseek(logfd, 0, SEEK_END) + 1;
 assert(index > 0); char event = 'x'; assert(write(logfd, &event, 1) == 1);
 if (index == fail_at) return -1; /* original child maps failed exec to 127 */
 if (index == signal_at) { raise(SIGKILL); _exit(99); }
 _exit(0);
}
#define sysctlbyname mock_sysctlbyname
#define execv mock_execv
#define main control_main
#include "@CONTROL@"
#undef main
#undef execv
int main(void) {
 logfd = open("exec-journal", O_CREAT | O_TRUNC | O_RDWR, 0600); assert(logfd >= 0);
 assert(control_main() == 0);
 int steps = (int)lseek(logfd, 0, SEEK_END); assert(steps >= 5);
 for (int i = 1; i <= steps; i++) {
  panel_calls = 0; assert(ftruncate(logfd, 0) == 0); fail_at = i; signal_at = 0;
  assert(control_main() != 0); assert(lseek(logfd, 0, SEEK_END) == i);
  panel_calls = 0; assert(ftruncate(logfd, 0) == 0); fail_at = 0; signal_at = i;
  assert(control_main() != 0); assert(lseek(logfd, 0, SEEK_END) == i);
 }
 fail_at = signal_at = 0;
 for (int at = 1; at <= 3; at++) for (panel_mode = 0; panel_mode < 4; panel_mode++) {
  panel_calls = 0; panel_bad_at = at; assert(ftruncate(logfd, 0) == 0);
  assert(control_main() != 0);
  assert(lseek(logfd, 0, SEEK_END) == (at == 1 ? 2 : at == 2 ? 4 : 6));
 }
 close(logfd);
 printf("PASS actual control.c: %d steps, every exec failure/signal stops subsequent actions; no actual commands ran\n", steps);
 return 0;
}
'''.replace('@CONTROL@', str(HERE / 'control.c'))
(build / 'control_host.c').write_text(source)
env = dict(os.environ, TMPDIR=str(build))
subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                str(build / 'control_host.c'), '-o', str(build / 'control_host')],
               cwd=build, env=env, check=True)
subprocess.run([str(build / 'control_host')], cwd=build, env=env, check=True)

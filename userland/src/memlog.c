/*
 * memlog SECONDS: every 10 s on the console, the phone's memory and who holds it, and then it goes on by itself so that
 * a job's next line runs: which process a jetsam of VM page shortage (OS_REASON_JETSAM code 4) leaves for last is
 * decided by what this shows (docs/KNOWN_ISSUES.md K40). In a test root only (mk_root.sh MEMLOG=1).
 */
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <unistd.h>

extern int proc_listallpids(void *buffer, int size);
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);
extern int proc_name(int pid, void *buffer, uint32_t size);

struct holder {
	int pid;
	uint64_t footprint, wired;
};

int
main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 300, console;
	static int pids[2048];
	static struct holder all[2048];

	if (fork() != 0) {
		return 0;
	}
	console = open("/dev/console", O_WRONLY | O_NOCTTY);
	for (int t = 0; t < seconds; t += 10) {
		vm_statistics64_data_t phone;
		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
		char line[900];
		int used = 0, n = proc_listallpids(pids, sizeof(pids)), held = 0;
		uint64_t sum = 0;

		if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&phone, &count) == KERN_SUCCESS) {
			used += snprintf(line + used, sizeof(line) - (size_t)used,
			    "hk-mem: free %llu active %llu inactive %llu wired %llu compressor %llu MB;",
			    (unsigned long long)phone.free_count * vm_kernel_page_size >> 20,
			    (unsigned long long)phone.active_count * vm_kernel_page_size >> 20,
			    (unsigned long long)phone.inactive_count * vm_kernel_page_size >> 20,
			    (unsigned long long)phone.wire_count * vm_kernel_page_size >> 20,
			    (unsigned long long)phone.compressor_page_count * vm_kernel_page_size >> 20);
		}
		{
			/* (a RAM boot's /var is 1536 MB of the kernel's memory whatever it holds, hk_ufs.c: how full) */
			struct statfs var;

			if (statfs("/private/var", &var) == 0) {
				used += snprintf(line + used, sizeof(line) - (size_t)used, " /var %llu of %llu MB;",
				    (unsigned long long)(var.f_blocks - var.f_bfree) * var.f_bsize >> 20,
				    (unsigned long long)var.f_blocks * var.f_bsize >> 20);
			}
		}
		for (int i = 0; i < n && held < 2048; i++) {
			struct rusage_info_v0 usage;

			if (pids[i] > 0 && proc_pid_rusage(pids[i], RUSAGE_INFO_V0, (rusage_info_t *)&usage) == 0) {
				all[held++] = (struct holder){ pids[i], usage.ri_phys_footprint, usage.ri_wired_size };
				sum += usage.ri_phys_footprint;
			}
		}
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %d processes %llu MB:", held, (unsigned long long)sum >> 20);
		for (int rank = 0; rank < 9 && used < (int)sizeof(line) - 60; rank++) {
			int top = -1;
			char name[40] = "?";

			for (int i = 0; i < held; i++) {
				top = all[i].pid != 0 && (top < 0 || all[i].footprint > all[top].footprint) ? i : top;
			}
			if (top < 0) {
				break;
			}
			(void)proc_name(all[top].pid, name, sizeof(name));
			used += snprintf(line + used, sizeof(line) - (size_t)used, " %s[%d] %llu", name, all[top].pid,
			    (unsigned long long)all[top].footprint >> 20);
			if (all[top].wired >> 20 > 0) {
				used += snprintf(line + used, sizeof(line) - (size_t)used, "+%llu", (unsigned long long)all[top].wired >> 20);
			}
			all[top].pid = 0;
		}
		line[used++] = '\n';
		if (console >= 0) {
			(void)!write(console, line, (size_t)used);
		}
		sleep(10);
	}
	return 0;
}

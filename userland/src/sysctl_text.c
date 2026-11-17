/*
 * sysctl_text NAME...: a text sysctl's value, on standard output, a line each ("NAME: value", or what refused it).
 *
 * For the kernel's diagnostics that are a sentence (kern.hackios.bt, kern.hackios.tz ...) in a job list, where there is no
 * shell and no sysctl(8). Reading one may be what makes the kernel ask: kern.hackios.tz asks TrustZone what it has running
 * the first time it is read (tools/nf/cell/hk_tz.c, boot-arg tz=). Reads only.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

int
main(int argc, char **argv)
{
	int failed = 0;

	for (int i = 1; i < argc; i++) {
		char text[2048];
		size_t size = sizeof(text) - 1;

		if (sysctlbyname(argv[i], text, &size, NULL, 0) != 0) {
			printf("%s: not read: %s\n", argv[i], strerror(errno));
			failed = 1;
			continue;
		}
		text[size] = 0;
		printf("%s: %s%s", argv[i], text, size > 0 && text[strlen(text) - 1] == '\n' ? "" : "\n");
	}
	return failed;
}

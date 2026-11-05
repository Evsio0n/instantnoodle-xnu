/*
 * files_drop: the files of a folder copied into Files' "On My iPhone" (the LocalStorage file provider's app-group
 * container, group.com.apple.FileProvider.LocalStorage, its "File Provider Storage"), for tests that need a file to
 * open in Files -- the first: the owner's HDR photos for backlog item 17 (2026-11-04, the owner: "妳可以放file裡面"),
 * on a RAM run's /var only. The container is found by its metadata (containermanagerd makes it, with a UUID of its
 * own, the first time Files runs). Copies are owned by mobile and named file-N.EXT (the extension kept); it says how
 * many went, never the names they had. With "open", the first is opened in Files (Quick Look) by a shareddocuments://
 * URL, through battery_test (whose entitlement lets SpringBoard open it).
 *
 * usage: files_drop FROM_DIR [open]
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_GROUPS      "/private/var/mobile/Containers/Shared/AppGroup"
#define LOCAL_STORAGE   "group.com.apple.FileProvider.LocalStorage"

/* whether the container's metadata names the identifier (it is in the plist as it is, binary or not) */
static int
names(const char *metadata, const char *identifier)
{
	static char buf[65536];
	int fd = open(metadata, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		return 0;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return 0;
	}
	buf[n] = 0;
	return memmem(buf, (size_t)n, identifier, strlen(identifier)) != NULL;
}

static int
copy(const char *from, const char *to)
{
	static char buf[1 << 16];
	int in = open(from, O_RDONLY), out;
	ssize_t n;

	if (in < 0) {
		return 0;
	}
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		close(in);
		return 0;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
	}
	close(in);
	fchown(out, 501, 501);
	close(out);
	return n == 0;
}

int
main(int argc, char **argv)
{
	char storage[1024] = "", path[1024], target[1200];
	DIR *dir;
	struct dirent *e;
	char first[1200] = "";
	unsigned copied = 0, failed = 0;

	if (argc != 2 && !(argc == 3 && strcmp(argv[2], "open") == 0)) {
		fprintf(stderr, "usage: files_drop FROM_DIR [open]\n");
		return 2;
	}
	if ((dir = opendir(APP_GROUPS)) == NULL) {
		printf("files_drop: no %s (Files has not run yet?)\n", APP_GROUPS);
		return 1;
	}
	while ((e = readdir(dir)) != NULL && storage[0] == 0) {
		if (e->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), APP_GROUPS "/%s/.com.apple.mobile_container_manager.metadata.plist", e->d_name);
		if (names(path, LOCAL_STORAGE)) {
			snprintf(storage, sizeof(storage), APP_GROUPS "/%s/File Provider Storage", e->d_name);
		}
	}
	closedir(dir);
	if (storage[0] == 0) {
		printf("files_drop: no container for %s (Files has not run yet?)\n", LOCAL_STORAGE);
		return 1;
	}
	mkdir(storage, 0755);
	chown(storage, 501, 501);
	if ((dir = opendir(argv[1])) == NULL) {
		printf("files_drop: nothing to copy\n");
		return 1;
	}
	while ((e = readdir(dir)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		const char *dot = strrchr(e->d_name, '.');

		snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
		snprintf(target, sizeof(target), "%s/file-%u%s", storage, copied + failed + 1, dot != NULL ? dot : "");
		if (copy(path, target)) {
			copied++;
			if (first[0] == 0) {
				strlcpy(first, target, sizeof(first));
			}
		} else {
			failed++;
		}
	}
	closedir(dir);
	printf("files_drop: %u file(s) put in On My iPhone, %u could not be\n", copied, failed);
	fflush(stdout);
	if (argc == 3 && first[0] != 0) {
		char url[2048] = "shareddocuments://";
		size_t n = strlen(url);

		for (const char *c = first; *c != 0 && n + 4 < sizeof(url); c++) {
			if (*c == ' ') {
				memcpy(url + n, "%20", 3);
				n += 3;
			} else {
				url[n++] = *c;
			}
		}
		url[n] = 0;
		execl("/hackios/bin/battery_test", "battery_test", "open", url, (char *)NULL);
		printf("files_drop: battery_test could not be started\n");
		return 1;
	}
	return failed != 0;
}

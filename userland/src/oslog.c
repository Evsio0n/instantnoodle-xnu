/*
 * oslog SECONDS [NAME...] [-m WORD]... [-x WORD]...: iOS's own log (os_log) as it is written, for SECONDS, on standard
 * output: the lines of the processes whose name contains one of NAME (of every process, with no NAME), info and debug lines
 * included; with -m only the lines that have one of those words in them, with -x none that has one of these (the agent's
 * exec returns a bounded amount of output, and a daemon's debug lines fill it in a moment: run 071113). For finding out
 * why a daemon of iOS decides what it decides without changing its job (OS_ACTIVITY_DT_MODE needs the job's plist, and a
 * service that launchd starts from inside a framework, as IMDPersistenceAgent, has none of its own).
 *
 * How: the log's live stream, the one Xcode and "log stream" read (LoggingSupport.framework, os_activity_stream_for_pid
 * with pid -1; the declarations are LLVM's lldb/tools/debugserver/source/MacOSX/DarwinLog/ActivityStreamSPI.h). diagnosticd
 * serves it and gives it to a client that holds com.apple.private.logging.stream: src/oslog.entitlements, signed by
 * build.sh with ldid. What a daemon logs as private stays "<private>".
 *
 * A run of seven or more digits is printed as "(digits)": a telephone number or an identity that a daemon logs publicly
 * does not get into a file through this tool.
 *
 * A diagnostic tool: nothing here changes the system. In a test root (or run by hand through the agent: iphont exec).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct stream;
struct event;

struct log_message {
	uint64_t trace_id;
	uint64_t timestamp;
	uint64_t thread;
	const uint8_t *image_uuid;
	const char *image_path;
	struct timeval tv_gmt;
	struct timezone tz;
	uint32_t offset;
	const char *format;
	const uint8_t *buffer;
	size_t buffer_sz;
	const uint8_t *privdata;
	size_t privdata_sz;
	const char *subsystem;
	const char *category;
	uint32_t oversize_id;
	uint8_t ttl;
	bool persisted;
};

struct entry {
	uint32_t type;
	pid_t pid;
	uint64_t proc_id;
	const uint8_t *proc_imageuuid;
	const char *proc_imagepath;
	uint64_t activity_id;
	uint64_t parent_id;
	struct log_message log_message;         /* (a union in the header; this is its member for the two types printed) */
};

enum {
	STREAM_DEBUG = 0x20, STREAM_INFO = 0x100,
	TYPE_LOG_MESSAGE = 0x0400, TYPE_LEGACY_LOG_MESSAGE = 0x0480,
};

static int names, musts, nots;
static char *name[32], *must[32], *not[32];
static unsigned long printed, seen;

int
main(int argc, char **argv)
{
	void *lib = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	struct stream *(*for_pid)(pid_t, uint32_t, bool (^)(struct entry *, int)) = lib != NULL ? dlsym(lib, "os_activity_stream_for_pid") : NULL;
	void (*resume)(struct stream *) = lib != NULL ? dlsym(lib, "os_activity_stream_resume") : NULL;
	void (*set_event_handler)(struct stream *, void (^)(struct stream *, struct event *)) = lib != NULL ? dlsym(lib, "os_activity_stream_set_event_handler") : NULL;
	char *(*formatted)(struct log_message *) = lib != NULL ? dlsym(lib, "os_log_copy_formatted_message") : NULL;
	int seconds = argc >= 2 ? atoi(argv[1]) : 0;
	struct stream *s;

	if (seconds <= 0) {
		fprintf(stderr, "usage: oslog SECONDS [NAME...] [-m WORD]... [-x WORD]...\n");
		return 2;
	}
	if (for_pid == NULL || resume == NULL || formatted == NULL) {
		printf("oslog: LoggingSupport has no stream to give (%s)\n", lib == NULL ? dlerror() : "a function is missing");
		return 1;
	}
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-m") == 0 && i + 1 < argc && musts < 32) {
			must[musts++] = argv[++i];
		} else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc && nots < 32) {
			not[nots++] = argv[++i];
		} else if (names < 32) {
			name[names++] = argv[i];
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	s = for_pid(-1, STREAM_INFO | STREAM_DEBUG, ^bool (struct entry *e, int error) {
		const char *path, *base;
		char *text;
		bool wanted = names == 0;
		struct tm tm;
		time_t at;
		int run = 0;

		if (error != 0 || e == NULL) {
			printf("oslog: the stream says error %d\n", error);
			return true;
		}
		seen++;
		if (e->type != TYPE_LOG_MESSAGE && e->type != TYPE_LEGACY_LOG_MESSAGE) {
			return true;
		}
		path = e->proc_imagepath != NULL ? e->proc_imagepath : "?";
		base = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
		for (int i = 0; i < names && !wanted; i++) {
			wanted = strstr(base, name[i]) != NULL;
		}
		if (!wanted) {
			return true;
		}
		text = formatted(&e->log_message);
		if (text != NULL && (musts > 0 || nots > 0)) {
			bool keep = musts == 0;

			for (int i = 0; i < musts && !keep; i++) {
				keep = strcasestr(text, must[i]) != NULL;
			}
			for (int i = 0; i < nots && keep; i++) {
				keep = strcasestr(text, not[i]) == NULL;
			}
			if (!keep) {
				free(text);
				return true;
			}
		}
		at = e->log_message.tv_gmt.tv_sec;
		localtime_r(&at, &tm);
		printf("%02d:%02d:%02d.%03d %s[%d] %s:%s ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(e->log_message.tv_gmt.tv_usec / 1000), base, e->pid,
		    e->log_message.subsystem != NULL ? e->log_message.subsystem : "-", e->log_message.category != NULL ? e->log_message.category : "-");
		/* (the text, with a long run of digits not printed and a line break as " | ") */
		for (const char *c = text != NULL ? text : "(no text)"; *c != 0; c++) {
			if (*c >= '0' && *c <= '9') {
				const char *end = c;

				while (*end >= '0' && *end <= '9') {
					end++;
				}
				run = (int)(end - c);
				if (run >= 7) {
					fputs("(digits)", stdout);
					c = end - 1;
					continue;
				}
				fwrite(c, 1, (size_t)run, stdout);
				c = end - 1;
				continue;
			}
			if (*c == '\n') {
				fputs(" | ", stdout);
			} else {
				fputc(*c, stdout);
			}
		}
		fputc('\n', stdout);
		free(text);
		printed++;
		return true;
	});
	if (s == NULL) {
		printf("oslog: no stream (diagnosticd does not give one)\n");
		return 1;
	}
	if (set_event_handler != NULL) {
		set_event_handler(s, ^(struct stream *stream, struct event *event) {
			(void)stream;
			printf("oslog: the stream's state changed (%p)\n", (void *)event);
		});
	}
	resume(s);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		printf("oslog: %d s: %lu line(s) printed of %lu entries seen\n", seconds, printed, seen);
		exit(0);
	});
	dispatch_main();
}

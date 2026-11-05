/*
 * threadsof: where another program's threads are, from outside it. An application extension that launchd kills every 5 s
 * for hanging on launch (CollectionsPoster, the wallpaper's: M5e) never loads this project's libraries and says nothing,
 * so what it is waiting for can only be read from here: task_for_pid, each thread's registers, and its frame pointers.
 *
 *   threadsof NAME [AGE [TIMES]]    goes into the background (a job list goes on). Whenever a process whose name begins
 *                                   with NAME has lived AGE seconds (default 3), its threads are said once, for TIMES
 *                                   processes (default 3).
 *
 *     threadsof: NAME[PID] N threads, M images
 *     threadsof: #T state S, x16 N: IMAGE+0xOFF IMAGE+0xOFF ... (leaf first)
 *     threadsof: #T names: SYMBOL SYMBOL ...
 *
 * The names come second, from a child that dlopens the images those addresses are in (the shared cache is at the same place
 * in every process), because opening a framework here can go wrong and the addresses must already be out by then. A
 * symbol is the nearest one exported, not necessarily the function.
 */
#include <dlfcn.h>
#include <mach-o/dyld_images.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOST_FRAMES 24
#define MOST_IMAGES 1600

struct image {
	uint64_t base;
	char path[160];
};

static struct image images[MOST_IMAGES];
static unsigned image_count;

static bool
peek(task_t task, uint64_t address, void *out, size_t size)
{
	vm_size_t got = 0;

	return vm_read_overwrite(task, (vm_address_t)address, size, (vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}

static int
by_base(const void *a, const void *b)
{
	const uint64_t x = ((const struct image *)a)->base, y = ((const struct image *)b)->base;

	return x < y ? -1 : x > y;
}

static void
read_images(task_t task)
{
	struct task_dyld_info info;
	mach_msg_type_number_t n = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos all;

	image_count = 0;
	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &n) != KERN_SUCCESS ||
	    !peek(task, info.all_image_info_addr, &all, sizeof(all))) {
		return;
	}
	for (uint32_t i = 0; i < all.infoArrayCount && image_count < MOST_IMAGES; i++) {
		struct dyld_image_info one;
		struct image *image = &images[image_count];

		if (!peek(task, (uint64_t)all.infoArray + i * sizeof(one), &one, sizeof(one))) {
			break;
		}
		image->base = (uint64_t)one.imageLoadAddress;
		if (!peek(task, (uint64_t)one.imageFilePath, image->path, sizeof(image->path))) {
			strcpy(image->path, "?");
		}
		image->path[sizeof(image->path) - 1] = 0;
		image_count++;
	}
	if (image_count < MOST_IMAGES) {
		images[image_count].base = (uint64_t)all.dyldImageLoadAddress;
		strcpy(images[image_count++].path, "dyld");
	}
	qsort(images, image_count, sizeof(images[0]), by_base);
}

static const struct image *
image_of(uint64_t pc)
{
	const struct image *best = NULL;

	for (unsigned i = 0; i < image_count && images[i].base <= pc; i++) {
		best = &images[i];
	}
	return best;
}

static void
say_threads(task_t task, const char *name, pid_t pid)
{
	thread_act_array_t threads = NULL;
	mach_msg_type_number_t count = 0;
	static uint64_t frames[64][MOST_FRAMES];
	static unsigned depth[64];

	read_images(task);
	if (task_threads(task, &threads, &count) != KERN_SUCCESS) {
		printf("threadsof: %s[%d]: its threads cannot be had\n", name, pid);
		return;
	}
	printf("threadsof: %s[%d] %u threads, %u images\n", name, pid, count, image_count);
	for (mach_msg_type_number_t t = 0; t < count && t < 64; t++) {
		arm_thread_state64_t state;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		struct thread_basic_info basic;
		mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT;
		char line[1400];
		size_t at = 0;

		depth[t] = 0;
		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&state, &n) != KERN_SUCCESS) {
			printf("threadsof: #%u no registers\n", t);
			continue;
		}
		basic.run_state = 0;
		thread_info(threads[t], THREAD_BASIC_INFO, (thread_info_t)&basic, &bn);
		at += (size_t)snprintf(line, sizeof(line), "threadsof: #%u state %d, x16 %lld:", t, basic.run_state,
		    (long long)state.__x[16]);

		uint64_t pc = __darwin_arm_thread_state64_get_pc(state) & 0x0000000fffffffffull;
		uint64_t lr = __darwin_arm_thread_state64_get_lr(state) & 0x0000000fffffffffull;
		uint64_t fp = __darwin_arm_thread_state64_get_fp(state);

		frames[t][depth[t]++] = pc;
		if (lr != 0) {
			frames[t][depth[t]++] = lr;
		}
		while (depth[t] < MOST_FRAMES && fp != 0 && (fp & 7) == 0) {
			uint64_t pair[2];

			if (!peek(task, fp, pair, sizeof(pair))) {
				break;
			}
			pair[1] &= 0x0000000fffffffffull;
			if (pair[1] == 0) {
				break;
			}
			frames[t][depth[t]++] = pair[1];
			fp = pair[0];
		}
		for (unsigned i = 0; i < depth[t] && at + 80 < sizeof(line); i++) {
			const struct image *image = image_of(frames[t][i]);
			const char *leaf = image != NULL ? strrchr(image->path, '/') : NULL;

			at += (size_t)snprintf(line + at, sizeof(line) - at, " %s+0x%llx",
			    image == NULL ? "?" : leaf != NULL ? leaf + 1 : image->path,
			    (unsigned long long)(frames[t][i] - (image != NULL ? image->base : 0)));
		}
		printf("%s\n", line);
	}
	fflush(stdout);
	/* the names, from a child that can come to harm opening these */
	if (fork() == 0) {
		for (mach_msg_type_number_t t = 0; t < count && t < 64; t++) {
			char line[1400];
			size_t at = (size_t)snprintf(line, sizeof(line), "threadsof: #%u names:", t);

			for (unsigned i = 0; i < depth[t] && at + 80 < sizeof(line); i++) {
				const struct image *image = image_of(frames[t][i]);
				Dl_info where = { 0 };

				if (image != NULL && image->path[0] == '/') {
					dlopen(image->path, RTLD_LAZY | RTLD_LOCAL);
				}
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %s",
				    dladdr((const void *)frames[t][i], &where) && where.dli_sname != NULL ? where.dli_sname : "?");
			}
			printf("%s\n", line);
			fflush(stdout);
		}
		_exit(0);
	}
	wait(NULL);
}

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";
	const long age = argc > 2 ? atol(argv[2]) : 3;
	int times = argc > 3 ? atoi(argv[3]) : 3;
	pid_t said[16] = { 0 };

	if (*name == 0) {
		printf("threadsof: threadsof NAME [AGE [TIMES]]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (fork() != 0) {
		return 0;
	}
	setsid();
	for (int said_count = 0; said_count < times && said_count < 16;) {
		int query[] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
		size_t size = 0;
		struct timeval now;

		sleep(1);
		gettimeofday(&now, NULL);
		if (sysctl(query, 3, NULL, &size, NULL, 0) != 0) {
			continue;
		}
		size += 16 * sizeof(struct kinfo_proc);

		struct kinfo_proc *all = malloc(size);

		if (all == NULL || sysctl(query, 3, all, &size, NULL, 0) != 0) {
			free(all);
			continue;
		}
		for (size_t i = 0; i < size / sizeof(*all) && said_count < times && said_count < 16; i++) {
			const pid_t pid = all[i].kp_proc.p_pid;
			bool done = pid == getpid();
			task_t task = MACH_PORT_NULL;

			for (int k = 0; k < said_count; k++) {
				done = done || said[k] == pid;
			}
			if (done || strncmp(all[i].kp_proc.p_comm, name, strlen(name)) != 0 ||
			    now.tv_sec - all[i].kp_proc.p_starttime.tv_sec < age) {
				continue;
			}
			said[said_count++] = pid;

			kern_return_t got = task_for_pid(mach_task_self(), pid, &task);

			if (got != KERN_SUCCESS) {
				printf("threadsof: %s[%d]: task_for_pid says %d\n", name, pid, got);
				continue;
			}
			say_threads(task, name, pid);
			mach_port_deallocate(mach_task_self(), task);
		}
		free(all);
	}
	printf("threadsof: done\n");
	return 0;
}

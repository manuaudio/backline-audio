/* slowdisk.so: an LD_PRELOAD shim that makes file writes stall, for the
 * recorder's "a slow disk must not cost a frame" proof.
 * SPDX-License-Identifier: MIT
 *
 *   SLOWDISK_PREFIX=/path/to/takes/    only writes to files under this path stall
 *   SLOWDISK_STALLS="3:12,20:8"        stall windows: START:SECONDS, from the first
 *                                      matching write; a write inside a window
 *                                      blocks until the window ends
 *   SLOWDISK_LOG=/path/log             one line per stall (start, duration, thread)
 *
 * It wraps write, pwrite, pwrite64, writev and fsync/fdatasync (and open and
 * close, to know which fds are under the prefix), which is
 * everything libsndfile and the recorder use to write a take. The calls block
 * in the calling thread exactly like a disk that stops answering (a USB drive
 * spinning up, a full NFS server, a thermal-throttled SSD).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_WIN 16

static struct { double start, len; int done; } win[MAX_WIN];
static int n_win = -1;
static double t0 = -1;
static char prefix[PATH_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void init(void)
{
	if (n_win >= 0)
		return;
	n_win = 0;
	const char *p = getenv("SLOWDISK_PREFIX"), *s = getenv("SLOWDISK_STALLS");
	snprintf(prefix, sizeof(prefix), "%s", p ? p : "");
	while (s && *s && n_win < MAX_WIN) {
		char *e;
		win[n_win].start = strtod(s, &e);
		if (*e != ':')
			break;
		win[n_win].len = strtod(e + 1, &e);
		n_win++;
		s = *e == ',' ? e + 1 : e;
	}
}

/* fds opened under the prefix; filled by the open wrappers, so a write
 * to anything else (PipeWire's eventfds on the data thread) costs one
 * array read and never a syscall or a lock. */
#define MAX_FD 4096
static volatile unsigned char tracked[MAX_FD];

static int matches(int fd)
{
	return fd >= 0 && fd < MAX_FD && tracked[fd];
}

static void track(int fd, const char *path)
{
	if (fd < 0 || fd >= MAX_FD)
		return;
	pthread_mutex_lock(&lock);
	init();
	tracked[fd] = prefix[0] && path && strncmp(path, prefix, strlen(prefix)) == 0;
	pthread_mutex_unlock(&lock);
}

static void maybe_stall(int fd)
{
	if (!matches(fd))
		return;
	pthread_mutex_lock(&lock);
	init();
	if (!n_win) {
		pthread_mutex_unlock(&lock);
		return;
	}
	double t = now();
	if (t0 < 0)
		t0 = t;
	double rel = t - t0, until = 0;
	for (int i = 0; i < n_win; i++)
		if (!win[i].done && rel >= win[i].start) {
			until = t0 + win[i].start + win[i].len;
			win[i].done = 1;
			const char *log = getenv("SLOWDISK_LOG");
			FILE *f = log ? fopen(log, "a") : NULL;
			if (f) {
				fprintf(f, "stall %d: at %.3f s for %.3f s (until %.3f s), thread %ld\n", i + 1, rel,
					until - t, until - t0, (long)syscall(SYS_gettid));
				fclose(f);
			}
		}
	pthread_mutex_unlock(&lock);
	while (until > 0 && now() < until) {
		struct timespec nap = { 0, 20 * 1000000L };
		nanosleep(&nap, NULL);
	}
}

#define REAL(name, ret, ...) \
	static ret (*real_##name)(__VA_ARGS__); \
	if (!real_##name) real_##name = (ret (*)(__VA_ARGS__))dlsym(RTLD_NEXT, #name)

ssize_t write(int fd, const void *buf, size_t n)
{
	REAL(write, ssize_t, int, const void *, size_t);
	maybe_stall(fd);
	return real_write(fd, buf, n);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	REAL(pwrite, ssize_t, int, const void *, size_t, off_t);
	maybe_stall(fd);
	return real_pwrite(fd, buf, n, off);
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off)
{
	REAL(pwrite64, ssize_t, int, const void *, size_t, off_t);
	maybe_stall(fd);
	return real_pwrite64(fd, buf, n, off);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{
	REAL(writev, ssize_t, int, const struct iovec *, int);
	maybe_stall(fd);
	return real_writev(fd, iov, cnt);
}

int fsync(int fd)
{
	REAL(fsync, int, int);
	maybe_stall(fd);
	return real_fsync(fd);
}

int fdatasync(int fd)
{
	REAL(fdatasync, int, int);
	maybe_stall(fd);
	return real_fdatasync(fd);
}

#define OPEN_MODE(flags, mode) \
	mode_t mode = 0; \
	if ((flags) & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }

int open(const char *path, int flags, ...)
{
	REAL(open, int, const char *, int, ...);
	OPEN_MODE(flags, mode);
	int fd = real_open(path, flags, mode);
	track(fd, path);
	return fd;
}

int open64(const char *path, int flags, ...)
{
	REAL(open64, int, const char *, int, ...);
	OPEN_MODE(flags, mode);
	int fd = real_open64(path, flags, mode);
	track(fd, path);
	return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
	REAL(openat, int, int, const char *, int, ...);
	OPEN_MODE(flags, mode);
	int fd = real_openat(dirfd, path, flags, mode);
	track(fd, path[0] == '/' ? path : NULL);
	return fd;
}

int close(int fd)
{
	REAL(close, int, int);
	if (fd >= 0 && fd < MAX_FD)
		tracked[fd] = 0;
	return real_close(fd);
}

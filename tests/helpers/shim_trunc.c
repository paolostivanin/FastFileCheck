// LD_PRELOAD: FFC_SHIM_TARGET=<path suffix>. When the target is opened read-only we remember the fd; the
// moment data is about to be read (first read() or right after mmap()) the file is truncated to 10 bytes.
// That is exactly the "file shrinks while we hash it" case that kills an mmap-based hasher with SIGBUS.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
static int trunc_fd = -1, fired = 0;
static char trunc_path[4096];
static void note (const char *path, int flags, int fd) {
    const char *t = getenv ("FFC_SHIM_TARGET");
    if (!t || fd < 0 || fired || trunc_fd >= 0 || (flags & (O_WRONLY | O_RDWR)) || !path) return;
    size_t pl = strlen (path), tl = strlen (t);
    if (pl >= tl && !strcmp (path + pl - tl, t) && pl < sizeof trunc_path) { trunc_fd = fd; strcpy (trunc_path, path); }
}
static void maybe_truncate (int fd) {
    if (fd == trunc_fd && !fired) { fired = 1; (void)!truncate (trunc_path, 10); }
}
#define WRAP_OPEN(name) int name (const char *p, int f, ...) { mode_t m = 0; if (f & O_CREAT) { va_list a; va_start (a, f); m = va_arg (a, mode_t); va_end (a); } \
    int (*r) (const char *, int, ...) = dlsym (RTLD_NEXT, #name); int fd = r (p, f, m); note (p, f, fd); return fd; }
#define WRAP_OPENAT(name) int name (int d, const char *p, int f, ...) { mode_t m = 0; if (f & O_CREAT) { va_list a; va_start (a, f); m = va_arg (a, mode_t); va_end (a); } \
    int (*r) (int, const char *, int, ...) = dlsym (RTLD_NEXT, #name); int fd = r (d, p, f, m); note (p, f, fd); return fd; }
WRAP_OPEN (open) WRAP_OPEN (open64) WRAP_OPENAT (openat) WRAP_OPENAT (openat64)
int __open_2 (const char *p, int f) { int (*r) (const char *, int) = dlsym (RTLD_NEXT, "__open_2"); int fd = r (p, f); note (p, f, fd); return fd; }
int __open64_2 (const char *p, int f) { int (*r) (const char *, int) = dlsym (RTLD_NEXT, "__open64_2"); int fd = r (p, f); note (p, f, fd); return fd; }
int __openat_2 (int d, const char *p, int f) { int (*r) (int, const char *, int) = dlsym (RTLD_NEXT, "__openat_2"); int fd = r (d, p, f); note (p, f, fd); return fd; }
int __openat64_2 (int d, const char *p, int f) { int (*r) (int, const char *, int) = dlsym (RTLD_NEXT, "__openat64_2"); int fd = r (d, p, f); note (p, f, fd); return fd; }
ssize_t read (int fd, void *b, size_t n) { maybe_truncate (fd); ssize_t (*r) (int, void *, size_t) = dlsym (RTLD_NEXT, "read"); return r (fd, b, n); }
ssize_t __read_chk (int fd, void *b, size_t n, size_t bl) { maybe_truncate (fd); ssize_t (*r) (int, void *, size_t, size_t) = dlsym (RTLD_NEXT, "__read_chk"); return r (fd, b, n, bl); }
void *mmap (void *a, size_t l, int p, int f, int fd, off_t o) { void *(*r) (void *, size_t, int, int, int, off_t) = dlsym (RTLD_NEXT, "mmap"); void *res = r (a, l, p, f, fd, o); maybe_truncate (fd); return res; }
void *mmap64 (void *a, size_t l, int p, int f, int fd, off_t o) { void *(*r) (void *, size_t, int, int, int, off_t) = dlsym (RTLD_NEXT, "mmap64"); void *res = r (a, l, p, f, fd, o); maybe_truncate (fd); return res; }

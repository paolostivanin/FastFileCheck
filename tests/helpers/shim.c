// LD_PRELOAD race injector. FFC_SHIM_TARGET=<path suffix>, FFC_SHIM_MODE=vanish|append|fifo
// On the FIRST read-open of a matching path it unlinks / appends a byte to / replaces it with a FIFO.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
static int fired = 0;
static void inject (const char *path, int flags) {
    const char *t = getenv ("FFC_SHIM_TARGET"), *m = getenv ("FFC_SHIM_MODE");
    if (!t || !m || fired || (flags & (O_WRONLY | O_RDWR)) || !path) return;
    size_t pl = strlen (path), tl = strlen (t);
    if (pl < tl || strcmp (path + pl - tl, t) != 0) return;
    fired = 1;
    if (!strcmp (m, "vanish")) unlink (path);
    else if (!strcmp (m, "fifo")) { unlink (path); mkfifo (path, 0644); }
    else if (!strcmp (m, "append")) {
        int (*real) (const char *, int, ...) = dlsym (RTLD_NEXT, "open");
        int fd = real (path, O_WRONLY | O_APPEND);
        if (fd >= 0) { (void)!write (fd, "!", 1); close (fd); }
    }
}
#define WRAP_OPEN(name) \
int name (const char *path, int flags, ...) { \
    mode_t mode = 0; if (flags & O_CREAT) { va_list ap; va_start (ap, flags); mode = va_arg (ap, mode_t); va_end (ap); } \
    inject (path, flags); \
    int (*real) (const char *, int, ...) = dlsym (RTLD_NEXT, #name); \
    return real (path, flags, mode); }
#define WRAP_OPENAT(name) \
int name (int dirfd, const char *path, int flags, ...) { \
    mode_t mode = 0; if (flags & O_CREAT) { va_list ap; va_start (ap, flags); mode = va_arg (ap, mode_t); va_end (ap); } \
    inject (path, flags); \
    int (*real) (int, const char *, int, ...) = dlsym (RTLD_NEXT, #name); \
    return real (dirfd, path, flags, mode); }
WRAP_OPEN (open) WRAP_OPEN (open64) WRAP_OPENAT (openat) WRAP_OPENAT (openat64)
int __open_2 (const char *path, int flags) { inject (path, flags); int (*r) (const char *, int) = dlsym (RTLD_NEXT, "__open_2"); return r (path, flags); }
int __open64_2 (const char *path, int flags) { inject (path, flags); int (*r) (const char *, int) = dlsym (RTLD_NEXT, "__open64_2"); return r (path, flags); }
int __openat_2 (int d, const char *path, int flags) { inject (path, flags); int (*r) (int, const char *, int) = dlsym (RTLD_NEXT, "__openat_2"); return r (d, path, flags); }
int __openat64_2 (int d, const char *path, int flags) { inject (path, flags); int (*r) (int, const char *, int) = dlsym (RTLD_NEXT, "__openat64_2"); return r (d, path, flags); }

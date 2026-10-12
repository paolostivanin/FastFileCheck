#pragma once

#include <glib.h>

// TRUE only if `path` lives on a block device that the kernel reports as rotational
// (/sys/class/block/<dev>/queue/rotational == 1). Unknown or non-block filesystems
// (tmpfs, NFS, overlayfs, FUSE, ...) are never reported as rotational.
//
// The path is mapped to its mount via /proc/self/mountinfo (longest mount-point prefix),
// not via st_dev: btrfs reports an anonymous st_dev per subvolume that is not a block device.
// Device-mapper/md devices are asked directly; their flag reflects the devices below them.
gboolean storage_path_is_rotational (const gchar *path);

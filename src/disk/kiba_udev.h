/* kiba_udev.h — waits for the kernel's view of newly-added partitions
 * to settle, without shelling out to `udevadm settle`.
 *
 * Why this is still needed even with BLKPG (see kiba_gpt.c): BLKPG
 * tells the kernel's block layer about the new partition immediately
 * and synchronously (the ioctl doesn't return until that's done) --
 * but *udev* (userspace) reacting to that change (creating the
 * /dev/vdaN symlink/device-node permissions, populating
 * /dev/disk/by-uuid/, etc.) is asynchronous and racy, exactly per the
 * upstream archinstall issues we found (#2286, #1759). Userspace tools
 * like blkid read from udev-populated state in some paths, so a short
 * poll-based wait here is the same fix as udevadm settle, just done by
 * directly polling sysfs instead of going through udev's own client
 * tool.
 */
#ifndef KIBA_UDEV_H
#define KIBA_UDEV_H

#include <stdbool.h>
#include <stddef.h>

/* Polls (no subprocess) until `path` (e.g. "/dev/vda1") exists AND
 * reports a stable, non-zero size (via BLKGETSIZE64 for block devices,
 * st_size otherwise), or `timeout_ms` elapses. Node existence alone is
 * not sufficient -- see kiba_udev.c for why. Returns true once the
 * device is both present and settled. */
bool kiba_wait_for_device(const char *path, int timeout_ms);

/* Polls blkid-equivalent state by repeatedly attempting to read the
 * given tag (e.g. "UUID" or "PARTUUID") for `part_path` directly from
 * /dev/disk/by-uuid and /dev/disk/by-partuuid symlinks (populated by
 * udev), without invoking blkid as a subprocess. Returns true and
 * fills `out_value` (caller-provided buffer of `out_len`) on success. */
bool kiba_wait_for_disk_tag(const char *part_path, const char *tag_dir_name,
                             char *out_value, size_t out_len, int timeout_ms);

/* Reads the PARTUUID of partition number `partno` (1-indexed) directly
 * out of the primary GPT partition entry array on `disk_path`, via
 * pread -- no udev, no blkid, no /dev/disk/by-partuuid dependency at
 * all. This is the preferred way to get a PARTUUID for a partition we
 * just created ourselves with kiba_gpt_write(), since we already know
 * exactly where to look on disk and don't need userspace device-node
 * population to have caught up.
 *
 * Returns true and fills out_value (format: lowercase hex with dashes,
 * matching blkid's PARTUUID= output) on success. */
bool kiba_read_partuuid_direct(const char *disk_path, int partno,
                                char *out_value, size_t out_len);

/* Forces the kernel to re-emit a "change" uevent for a partition device
 * by writing to its sysfs uevent file, which is what actually causes
 * udev to (re-)run its blkid probe and update /dev/disk/by-uuid,
 * /dev/disk/by-label, etc. This is the missing half of the story the
 * rest of this header solves: BLKPG/BLKRRPART at partition-table-write
 * time tells the kernel about a partition's existence, but writing an
 * actual filesystem into that partition afterward (mkfs.ext4, mkfs.fat)
 * doesn't itself trigger any uevent -- so without calling this right
 * after formatting, kiba_wait_for_disk_tag() below can end up polling a
 * by-uuid symlink that either never appears, or (on a disk that's been
 * formatted before) resolves to a stale UUID left over from whatever
 * filesystem used to be there. Call this once per partition immediately
 * after kiba_fs_format() succeeds, before reading its UUID back.
 * Returns true if the uevent was written; false just means the sysfs
 * node couldn't be opened (caller should treat that as "couldn't
 * confirm the retrigger", not necessarily fatal on its own). */
bool kiba_trigger_uevent(const char *part_path);

#endif

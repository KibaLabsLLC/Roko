/* kiba_udev.c — see kiba_udev.h. */
#define _GNU_SOURCE
#include "kiba_udev.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h> /* BLKGETSIZE64 */

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Node existence alone isn't enough: udev can materialize the dentry for
 * a freshly-created partition device node microseconds before the
 * kernel's block layer has finished wiring up that partition's actual
 * size -- BLKGETSIZE64 still reads 0 (or the node is openable but
 * "there" in name only) for a brief window right after the mknod. A
 * caller that proceeds the instant stat() succeeds can race ahead of
 * that and format/write against a device the kernel doesn't consider
 * fully live yet -- the same class of race kiba_gpt.c's BLKPG comment
 * describes, just one layer further down the stack. So this now polls
 * the actual reported size (via BLKGETSIZE64 for block devices, or
 * st_size for anything else, e.g. a loopback-backed regular file in a
 * test harness) and requires two consecutive non-zero reads of the
 * *same* size before calling the device ready -- a single non-zero
 * read could still be mid-transition on some drivers, so we want it to
 * have settled, not just briefly been non-zero once. */
bool kiba_wait_for_device(const char *path, int timeout_ms) {
    struct stat st;
    int waited = 0;
    const int step_ms = 100;
    uint64_t last_size = 0;
    int stable_reads = 0;

    while (waited <= timeout_ms) {
        if (stat(path, &st) == 0) {
            uint64_t size = 0;
            bool have_size = false;

            if (S_ISBLK(st.st_mode)) {
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    have_size = (ioctl(fd, BLKGETSIZE64, &size) == 0);
                    close(fd);
                }
            } else {
                size = (uint64_t)st.st_size;
                have_size = true;
            }

            if (have_size && size > 0) {
                if (size == last_size) {
                    if (++stable_reads >= 2) return true;
                } else {
                    last_size = size;
                    stable_reads = 1;
                }
            } else {
                stable_reads = 0;
                last_size = 0;
            }
        }
        sleep_ms(step_ms);
        waited += step_ms;
    }
    return false;
}

/* /dev/disk/by-uuid/<UUID> and /dev/disk/by-partuuid/<PARTUUID> are
 * symlinks udev creates pointing back at e.g. ../../vda1. We resolve
 * every entry in the requested directory and compare its target
 * against `part_path` (after resolving both to canonical form), which
 * gives us the tag value without ever invoking blkid. */
bool kiba_wait_for_disk_tag(const char *part_path, const char *tag_dir_name,
                             char *out_value, size_t out_len, int timeout_ms) {
    char real_part[PATH_MAX];
    if (!realpath(part_path, real_part)) return false;

    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/dev/disk/%s", tag_dir_name);

    int waited = 0;
    const int step_ms = 150;
    while (waited <= timeout_ms) {
        DIR *d = opendir(dir_path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char full[PATH_MAX];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                char resolved[PATH_MAX];
                if (realpath(full, resolved) && strcmp(resolved, real_part) == 0) {
                    snprintf(out_value, out_len, "%s", ent->d_name);
                    closedir(d);
                    return true;
                }
            }
            closedir(d);
        }
        sleep_ms(step_ms);
        waited += step_ms;
    }
    return false;
}

bool kiba_read_partuuid_direct(const char *disk_path, int partno,
                                char *out_value, size_t out_len) {
    if (partno < 1 || partno > 128) return false;

    int fd = open(disk_path, O_RDONLY);
    if (fd < 0) return false;

    /* Read the primary GPT header (LBA 1) to find sector size and the
     * partition entry array location -- we don't hardcode sector size
     * here so this also works correctly on 4Kn disks. */
    uint8_t sector_probe[4096];
    /* Try 512 first since BLKSSZGET requires an ioctl we'd rather avoid
     * duplicating here; the header's own self-description (its LBA is
     * always 1) lets us detect the real sector size by trying 512 and
     * checking the signature, falling back to 4096. */
    ssize_t r = pread(fd, sector_probe, 512, 512);
    uint32_t ssz;
    if (r == 512 && memcmp(sector_probe, "EFI PART", 8) == 0) {
        ssz = 512;
    } else {
        r = pread(fd, sector_probe, 4096, 4096);
        if (r == 4096 && memcmp(sector_probe, "EFI PART", 8) == 0) {
            ssz = 4096;
        } else {
            close(fd);
            return false;
        }
    }

    uint8_t hdr[512];
    if (pread(fd, hdr, sizeof(hdr), (off_t)ssz) != (ssize_t)sizeof(hdr)) {
        close(fd); return false;
    }
    uint64_t array_lba;
    uint32_t entry_size;
    memcpy(&array_lba, hdr + 72, 8);
    memcpy(&entry_size, hdr + 84, 4);

    off_t entry_off = (off_t)array_lba * ssz + (off_t)(partno - 1) * entry_size;
    uint8_t entry[128];
    if (entry_size < 32 || entry_size > sizeof(entry) ||
        pread(fd, entry, entry_size, entry_off) != (ssize_t)entry_size) {
        close(fd); return false;
    }
    close(fd);

    /* Unique partition GUID lives at bytes [16:32) of the entry, in the
     * same mixed-endian layout GPT uses everywhere: u32 LE, u16 LE,
     * u16 LE, then 8 raw bytes -- matching how blkid renders PARTUUID. */
    const uint8_t *g = entry + 16;
    snprintf(out_value, out_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0],
             g[5], g[4],
             g[7], g[6],
             g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
    return true;
}

bool kiba_trigger_uevent(const char *part_path) {
    /* Partition device paths are always a flat basename directly under
     * /dev (e.g. "/dev/vda1", "/dev/nvme0n1p1", "/dev/sda1") -- the
     * kernel exposes a matching flat entry for every block device
     * (whole-disk or partition) directly under /sys/class/block/ by
     * that same basename, no need to walk /sys/block/<disk>/<part>
     * separately or resolve any symlink ourselves first. */
    const char *slash = strrchr(part_path, '/');
    const char *name = slash ? slash + 1 : part_path;
    if (name[0] == '\0') return false;

    char sysfs_path[PATH_MAX];
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/block/%s/uevent", name);

    int fd = open(sysfs_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    /* "change" (not "add") -- the partition already exists as far as the
     * kernel/udev's device model is concerned; what changed is its
     * *content* (a filesystem got written where there wasn't one, or a
     * different one than before), which is exactly what the "change"
     * action means and what triggers udev's blkid rule to re-probe. */
    ssize_t w = write(fd, "change", 6);
    close(fd);
    return w == 6;
}

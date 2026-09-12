/* kiba_fs.h — filesystem creation + mounting, no shell involved anywhere.
 *
 * Honest scope note: there is no maintained C *library* for writing
 * FAT32 or ext4 from scratch that's lighter/safer than the reference
 * mkfs tools themselves (mkfs.fat, mkfs.ext4) — those tools ARE the
 * spec-compliant implementation maintained by the kernel/util-linux
 * communities. Reimplementing ext4's journal+extents+checksums by hand
 * here would trade a small, stable dependency for a large hand-written
 * one with real data-loss risk if subtly wrong.
 *
 * What this module removes instead is everything *fragile* about how
 * the previous backend called external tools:
 *   - no /bin/sh involved at any point (execvp, not system()/popen())
 *   - no stdout string-scraping for results — only exit status matters
 *   - no shell quoting of user-controlled strings (there are none here;
 *     mkfs only ever receives device paths and fixed flags)
 *   - mount(2) is called directly via syscall, not the `mount` binary
 *
 * The actual partitioning (kiba_gpt.c) has zero external dependencies.
 */
#ifndef KIBA_FS_H
#define KIBA_FS_H

#include <stdint.h>

typedef enum {
    KIBA_FS_FAT32,
    KIBA_FS_EXT4,
} kiba_fs_type_t;

/* Formats `part_path` (e.g. "/dev/vda1") with the given filesystem.
 * `volume_label` may be NULL. Returns 0 on success, -errno-ish on
 * failure (see kiba_fs_strerror for a human-readable string, since
 * exec failures don't map cleanly to errno alone). */
int kiba_fs_format(const char *part_path, kiba_fs_type_t type,
                    const char *volume_label);

/* Mounts `part_path` at `target_dir` with the given fstype ("vfat",
 * "ext4") and optional comma-free mount options string (or NULL).
 * Calls mount(2) directly — no `mount` binary involved. */
int kiba_fs_mount(const char *part_path, const char *target_dir,
                   const char *fstype, const char *options);

int kiba_fs_umount(const char *target_dir);

/* Returns a human-readable description of the last kiba_fs_* error on
 * this thread (mkfs exit code / signal, or strerror() for mount(2)). */
const char *kiba_fs_strerror(void);

#endif

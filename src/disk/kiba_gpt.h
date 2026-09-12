/* kiba_gpt.h — GPT partition table writer, backed by sgdisk.
 *
 * Previously this was a from-scratch, hand-rolled implementation of
 * UEFI Spec 2.10 chapter 5 (protective MBR + primary/backup GPT header +
 * entry array) written directly via pwrite(), with BLKPG ioctls standing
 * in for partprobe. That was later swapped for libfdisk's C API, which
 * pulled in a link-time dependency (-lfdisk) and a fair amount of
 * fdisk_context/fdisk_partition object plumbing for what's fundamentally
 * a handful of "add this partition" calls. This revision replaces that
 * API dependency with `sgdisk` (from the gptfdisk package) invoked as a
 * plain subprocess — same underlying correctness guarantee (sgdisk is
 * the same kind of reference GPT implementation libfdisk is, and is
 * what most distro installers already shell out to), but the call
 * surface is now just "build an argv array, exec it, check the exit
 * code" instead of a C library binding. Every invocation goes through
 * execvp() with a real argv array — never a shell — so partition names
 * or GUIDs containing spaces or shell metacharacters are passed through
 * literally with no quoting/injection surface, same guarantee the rest
 * of this backend already holds itself to.
 *
 * The layout math (where each partition starts/ends, accounting for the
 * GPT header + entry array at both ends of the disk) is still computed
 * by this code rather than left to sgdisk's own "-n 0:0:0" defaults, so
 * placement stays fully deterministic and doesn't require parsing
 * sgdisk's output back out to find out where it actually put things.
 *
 * The public API below is UNCHANGED from the previous version, so
 * kibaos_oobe_backend_main.c and every other caller needs zero changes.
 *
 * Build requirement: none beyond the `sgdisk` binary being on PATH at
 * runtime (package: gptfdisk, already in packages.x86_64). No extra
 * -l flag needed at link time — see the gcc invocation building
 * kibaos-oobe-backend.
 */
#ifndef KIBA_GPT_H
#define KIBA_GPT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 16-byte little/mixed-endian GUID, stored exactly as UEFI expects on disk.
 * Kept for ABI compatibility with existing callers — internally this is
 * now converted to/from the canonical string-GUID format sgdisk's -u/-U
 * flags expect. */
typedef struct {
    uint8_t b[16];
} kiba_guid_t;

/* Well-known partition type GUIDs (UEFI Spec 2.10 Table 5-7 + Linux conventions) */
extern const kiba_guid_t KIBA_GUID_ESP;          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const kiba_guid_t KIBA_GUID_LINUX_FS;     /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */

typedef struct {
    char        name[37];      /* NUL-terminated, displayed only; truncated to 36 UTF-16 chars on disk */
    kiba_guid_t type_guid;
    kiba_guid_t unique_guid;   /* if all-zero, sgdisk generates one */
    uint64_t    first_lba;     /* Pass KIBA_GPT_FIRST_LBA_DEFAULT to let this
                                 * code compute the first usable LBA itself
                                 * (only valid for the first partition in the
                                 * array) or KIBA_GPT_FIRST_LBA_CONTIGUOUS to
                                 * start right after the previous partition's
                                 * resolved on-disk placement. Either way, when
                                 * first_lba isn't a concrete number, last_lba
                                 * below is reinterpreted as a sector COUNT
                                 * rather than an absolute LBA, since the real
                                 * start (and therefore the real end) isn't
                                 * resolved until write time. */
    uint64_t    last_lba;      /* inclusive. Pass KIBA_GPT_LAST_LBA_REST to
                                 * consume all remaining space on the disk --
                                 * this uses this code's own computed
                                 * last-usable-LBA (which already accounts for
                                 * the backup GPT header + entry array at the
                                 * end of the disk) instead of recomputing it
                                 * by hand at every call site. */
    uint64_t    attributes;
} kiba_gpt_partition_t;

/* Sentinels for first_lba/last_lba: never legitimate LBA values (or sector
 * counts, for KIBA_GPT_FIRST_LBA_DEFAULT's reinterpretation of last_lba), so
 * safe to reuse as flags. The first/last usable LBA math (GPT header +
 * 128-entry array on each end of the disk, sized off the real logical
 * sector size) is centralized once in kiba_gpt_write() instead of being
 * duplicated by hand at every call site -- see the entry_array_sectors
 * calculation there. This is the same math sgdisk and libfdisk both use
 * internally for a standard 128-entry GPT, so callers land on exactly
 * the same first/last usable LBAs those tools would pick by default. */
#define KIBA_GPT_LAST_LBA_REST        UINT64_MAX
#define KIBA_GPT_FIRST_LBA_DEFAULT    UINT64_MAX
#define KIBA_GPT_FIRST_LBA_CONTIGUOUS (UINT64_MAX - 1)

typedef struct {
    int      fd;                 /* open O_RDWR on the whole-disk block device */
    uint32_t logical_sector_size;
    uint64_t total_sectors;
    kiba_guid_t disk_guid;       /* if all-zero, sgdisk generates one */
} kiba_gpt_disk_t;

/* Generates a random RFC-4122 v4 GUID using /dev/urandom — no external tool. */
kiba_guid_t kiba_guid_random(void);

/* Creates a fresh GPT label and writes the given partitions (in order) to
 * disk->fd by invoking sgdisk as a subprocess (single call, all -n/-t/-c/-u
 * options for every partition passed in one argv array). Returns 0 on
 * success, -errno on failure (sgdisk's own error is collapsed to -EIO,
 * since it reports failures via stderr text rather than a stable numeric
 * code — the message is left on stderr for anyone debugging a failed run).
 *
 * On success, the kernel partition table is updated for each partition
 * automatically (sgdisk issues the BLKPG/BLKRRPART reread itself) —
 * caller does not need to call partprobe.
 *
 * out_last_lba: optional (may be NULL). If non-NULL, must point to an
 * array of n_parts uint64_t -- filled in with each partition's resolved
 * on-disk last LBA, computed by this function before it ever calls
 * sgdisk. Needed whenever a later partition uses
 * KIBA_GPT_FIRST_LBA_CONTIGUOUS, so the next iteration knows where the
 * previous partition actually landed.
 *
 * NOTE: disk->fd is used only to derive the device path (via
 * /proc/self/fd) to pass to sgdisk on argv — sgdisk opens the device
 * itself. The fd passed in must stay open for the duration of the call.
 */
int kiba_gpt_write(kiba_gpt_disk_t *disk,
                    const kiba_gpt_partition_t *parts, size_t n_parts,
                    uint64_t *out_last_lba);

/* Reads back sector size + total size for `path` (e.g. "/dev/vda") via
 * ioctl (BLKSSZGET, BLKGETSIZE64) — no `blockdev`/`lsblk` subprocess.
 * (Unchanged — these ioctls were never the risky part.) */
int kiba_gpt_probe_device(const char *path, uint32_t *sector_size,
                           uint64_t *total_sectors);

/* ── Dual-boot / "install alongside" support ─────────────────────────
 * Everything above this point assumes we own the whole disk and are
 * free to lay down a brand-new GPT (kiba_gpt_write() runs `sgdisk -Z`
 * first, which wipes whatever was there). The functions below are the
 * non-destructive counterparts used when the disk already has an OS on
 * it that the user wants to keep. */

typedef struct {
    int      esp_partno;      /* 1-indexed partno of an existing ESP found
                                * on the disk, or 0 if none exists. */
    uint64_t esp_first_lba;
    uint64_t esp_last_lba;
    uint64_t free_first_lba;  /* largest contiguous run of unallocated
                                * sectors on the disk. Zero-length
                                * (free_last_lba < free_first_lba) if the
                                * disk has no usable free space. */
    uint64_t free_last_lba;
    uint64_t free_bytes;
} kiba_gpt_scan_result_t;

/* Reads the EXISTING partition table on `path` without modifying
 * anything on disk (runs `sgdisk -p`, read-only). Locates an existing
 * EFI System Partition, if any, and the single largest gap of
 * unallocated sectors, for dual-boot free-space installs. If the disk
 * has no valid GPT label at all, *out is zeroed and this returns 0 —
 * callers should treat that the same as "no free space / no ESP found"
 * rather than as an error, since an unpartitioned disk simply isn't a
 * dual-boot candidate. Returns -errno only if the device itself
 * couldn't be opened/read. */
int kiba_gpt_scan(const char *path, kiba_gpt_scan_result_t *out);

/* Adds ONE new partition to an EXISTING GPT table on `path` — unlike
 * kiba_gpt_write(), this does NOT run `sgdisk -Z`/-o and does NOT touch
 * any partition already on the disk. `part->first_lba`/`last_lba`
 * should fall inside a free region (normally taken straight from a
 * prior kiba_gpt_scan() call's free_first_lba/free_last_lba).
 * On success, writes the new partition's 1-indexed partition number to
 * *out_partno. Returns 0 on success, -EIO/-errno on failure. */
int kiba_gpt_add_partition(const char *path, const kiba_gpt_partition_t *part,
                            int *out_partno);

#endif

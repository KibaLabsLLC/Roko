/* kiba_gpt.c — GPT writer backed by sgdisk (gptfdisk), run as a subprocess.
 *
 * The previous revision of this file called into libfdisk's C API
 * (fdisk_new_context / fdisk_add_partition / etc). This revision drops
 * that library dependency entirely and instead builds a plain argv
 * array and hands it to sgdisk via fork()+execvp() -- never a shell, so
 * nothing here (partition names, GUIDs, device paths) needs escaping
 * regardless of its contents. The public API (kiba_gpt.h) is unchanged
 * -- all callers continue to work without modification.
 *
 * sgdisk still handles the parts that are genuinely easy to get subtly
 * wrong by hand:
 *   - Protective MBR
 *   - Primary + backup GPT headers (including CRC32)
 *   - Partition entry array
 *   - BLKPG / BLKRRPART kernel notification (sgdisk does this itself
 *     after a successful write, same as partprobe would)
 *
 * What THIS file now owns instead of deferring to the library: the
 * actual sector-placement math (first/last usable LBA, where each
 * partition starts/ends). Computing that ourselves means every call to
 * sgdisk is a single, fully-formed command with concrete numbers --
 * no "-n 0:0:0 let sgdisk pick" placeholders whose result would then
 * need to be read back out of sgdisk's text output just to know where
 * things landed.
 *
 * We retain our own kiba_gpt_probe_device() (raw ioctls, unchanged) and
 * kiba_guid_random() since neither has anything to do with sgdisk.
 */
#define _GNU_SOURCE
#include "kiba_gpt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>       /* strcasecmp */
#include <limits.h>        /* PATH_MAX */
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/fs.h>     /* BLKSSZGET, BLKGETSIZE64 */

/* ── Well-known type GUIDs (string form for sgdisk's -u/-U flags) ───── */
/* sgdisk accepts full GUIDs as canonical strings: 8-4-4-4-12 hex. For
 * partition *type* it also accepts short 4-hex-digit codes (ef00 = EFI
 * System, 8300 = Linux filesystem) which is what we pass to -t below --
 * these full-GUID #defines are only used for the type comparison against
 * caller-supplied kiba_guid_t values and for kiba_gpt_scan()'s output
 * parsing. */
#define KIBA_GUID_ESP_STR      "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define KIBA_GUID_LINUX_FS_STR "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define KIBA_TYPE_CODE_ESP       "ef00"
#define KIBA_TYPE_CODE_LINUX_FS  "8300"

/* Public kiba_guid_t constants — kept for API compatibility with callers
 * that compare against them. Byte layout: u32 LE, u16 LE, u16 LE, u8[8]. */
const kiba_guid_t KIBA_GUID_ESP = {
    .b = { 0x28,0x73,0x2a,0xc1, 0x1f,0xf8, 0xd2,0x11,
           0xba,0x4b, 0x00,0xa0,0xc9,0x3e,0xc9,0x3b }
};
const kiba_guid_t KIBA_GUID_LINUX_FS = {
    .b = { 0xaf,0x3d,0xc6,0x0f, 0x83,0x84, 0x72,0x47,
           0x8e,0x79, 0x3d,0x69,0xd8,0x47,0x7d,0xe4 }
};

/* ── GUID helpers ────────────────────────────────────────────────────── */
static bool guid_is_zero(const kiba_guid_t *g) {
    for (int i = 0; i < 16; i++) if (g->b[i]) return false;
    return true;
}

kiba_guid_t kiba_guid_random(void) {
    kiba_guid_t g;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(g.b, 1, 16, f) != 16) {
        for (int i = 0; i < 16; i++) g.b[i] = (uint8_t)(rand() & 0xFF);
    }
    if (f) fclose(f);
    /* RFC 4122 v4 bits */
    g.b[6] = (uint8_t)((g.b[6] & 0x0F) | 0x40);
    g.b[8] = (uint8_t)((g.b[8] & 0x3F) | 0x80);
    return g;
}

/* Convert our kiba_guid_t (mixed-endian on-disk bytes) to the canonical
 * 8-4-4-4-12 string that sgdisk's -u/-U flags expect.
 * GPT GUID wire format: first three groups are LE, last two are BE/raw. */
static void guid_to_str(const kiba_guid_t *g, char out[37]) {
    const uint8_t *b = g->b;
    snprintf(out, 37,
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        b[3],b[2],b[1],b[0],   /* u32 LE → big-endian display */
        b[5],b[4],              /* u16 LE → big-endian display */
        b[7],b[6],              /* u16 LE → big-endian display */
        b[8],b[9],              /* remaining 8 bytes raw */
        b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* ── Device probing via ioctl (unchanged from original) ─────────────── */
int kiba_gpt_probe_device(const char *path, uint32_t *sector_size,
                           uint64_t *total_sectors) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;

    int ssz = 0;
    if (ioctl(fd, BLKSSZGET, &ssz) != 0) { int e = errno; close(fd); return -e; }

    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) { int e = errno; close(fd); return -e; }

    close(fd);
    *sector_size   = (uint32_t)ssz;
    *total_sectors = bytes / (uint64_t)ssz;
    return 0;
}

/* ── Run sgdisk as a subprocess via a real argv array — never a shell.
 * Discards sgdisk's (very chatty) stdout; leaves stderr connected so a
 * real failure still shows up in the caller's logs. Returns 0 on a
 * clean exit, -ENOENT if the sgdisk binary itself couldn't be found,
 * -EIO if sgdisk ran but reported failure, or -errno if fork/wait
 * itself failed. */
static int run_sgdisk(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -errno;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        execvp("sgdisk", argv);
        _exit(127); /* only reached if execvp() itself failed */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -errno;
    if (!WIFEXITED(status)) return -EIO;
    int code = WEXITSTATUS(status);
    if (code == 127) return -ENOENT;
    return code == 0 ? 0 : -EIO;
}

/* Same as run_sgdisk(), but captures stdout into `out` (NUL-terminated,
 * truncated to out_sz - 1 bytes) instead of discarding it -- used by
 * kiba_gpt_scan(), which actually needs to read sgdisk's `-p` table
 * back out. Stderr is silenced here since sgdisk prints routine
 * "Creating new GPT entries in memory" notices there even on success. */
static int run_sgdisk_capture(char *const argv[], char *out, size_t out_sz) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -errno;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -errno; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp("sgdisk", argv);
        _exit(127);
    }
    close(pipefd[1]);

    size_t used = 0;
    ssize_t n;
    while (used < out_sz - 1 &&
           (n = read(pipefd[0], out + used, out_sz - 1 - used)) > 0) {
        used += (size_t)n;
    }
    out[used] = '\0';
    /* Drain any remainder so the child never blocks writing to a full pipe. */
    char scratch[256];
    while (read(pipefd[0], scratch, sizeof(scratch)) > 0) {}
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -errno;
    if (!WIFEXITED(status)) return -EIO;
    int code = WEXITSTATUS(status);
    if (code == 127) return -ENOENT;
    return code == 0 ? 0 : -EIO;
}

/* ── kiba_gpt_write — the sgdisk-subprocess implementation ──────────── */
int kiba_gpt_write(kiba_gpt_disk_t *disk,
                    const kiba_gpt_partition_t *parts, size_t n_parts,
                    uint64_t *out_last_lba) {
    if (n_parts == 0 || n_parts > 128) return -EINVAL;
    if (disk->logical_sector_size == 0) return -EINVAL;

    /* Resolve the block device path from our open fd via /proc/self/fd --
     * sgdisk needs a path string on argv, not an fd. */
    char fd_link[64];
    char dev_path[PATH_MAX];
    snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", disk->fd);
    ssize_t len = readlink(fd_link, dev_path, sizeof(dev_path) - 1);
    if (len < 0) return -errno;
    dev_path[len] = '\0';

    /* GPT layout constants -- the same math a standard 128-entry table
     * uses under sgdisk/libfdisk: entry array is 128 * 128 bytes,
     * rounded up to whole sectors, with one header sector at each end
     * of the disk (LBA 0 is the protective MBR; LBA 1 is the primary
     * header; the very last LBA is the backup header). Computed once
     * here instead of asking sgdisk where it put things afterward. */
    uint32_t ssz = disk->logical_sector_size;
    uint64_t entry_array_sectors = (16384 + ssz - 1) / ssz;
    uint64_t first_usable = 2 + entry_array_sectors;
    uint64_t last_usable  = disk->total_sectors - 2 - entry_array_sectors;

    /* Per-partition argv string storage -- heap-allocated since n_parts
     * isn't a compile-time constant. Every string here becomes a literal
     * argv element (no shell in between), so partition names containing
     * spaces or odd characters need no escaping at all. */
    struct { char n[48], t[24], c[80], u[56]; } *buf = calloc(n_parts, sizeof(*buf));
    if (!buf) return -ENOMEM;

    bool have_disk_guid = !guid_is_zero(&disk->disk_guid);
    char disk_guid_str[37];
    if (have_disk_guid) guid_to_str(&disk->disk_guid, disk_guid_str);

    /* Upper bound: sgdisk, -Z, -a, 1, [-U guid], (per part: -n v -t v [-c v] [-u v]), path, NULL */
    char *argv[8 + n_parts * 8 + 2];
    size_t ai = 0;
    argv[ai++] = "sgdisk";
    argv[ai++] = "-Z";                       /* wipe any existing MBR/GPT, start clean */
    /* -a 1: keep our LBAs exact. Without this, sgdisk uses its normal
     * 2048-sector (1MiB) alignment grid and silently snaps any explicit
     * numeric start passed via -n up to that grid -- and first_usable
     * above (LBA 34 for a standard 512-byte-sector 128-entry table) is
     * NOT itself a multiple of that grid. That meant partition 1's
     * *real* on-disk start could land ~2014 sectors later than the
     * first_usable value this function's own resolved_first/resolved_last
     * math was built on, while every partition after it was still placed
     * using the unsnapped numbers -- a genuine positional mismatch
     * between what we told the kernel and what sgdisk actually wrote,
     * not just a cosmetic rounding difference. That class of drift is
     * exactly what produces the kernel's in-core partition table
     * disagreeing with what's really on disk, which is what makes
     * mke2fs report a device size of zero right after formatting even
     * though the partition is right there. kiba_gpt_add_partition()
     * below already does this for exactly the same reason -- this just
     * brings kiba_gpt_write() in line with it, so out_last_lba is
     * always the truth, not merely what we asked for. */
    argv[ai++] = "-a"; argv[ai++] = "1";
    if (have_disk_guid) { argv[ai++] = "-U"; argv[ai++] = disk_guid_str; }

    uint64_t prev_end = 0;
    for (size_t i = 0; i < n_parts; i++) {
        const kiba_gpt_partition_t *p = &parts[i];

        bool first_is_default    = (p->first_lba == KIBA_GPT_FIRST_LBA_DEFAULT);
        bool first_is_contiguous = (p->first_lba == KIBA_GPT_FIRST_LBA_CONTIGUOUS);
        /* Per kiba_gpt.h: "Either way, when first_lba isn't a concrete
         * number, last_lba below is reinterpreted as a sector COUNT" --
         * that "either way" covers BOTH computed-start sentinels, not
         * just DEFAULT. Previously only DEFAULT actually got count
         * semantics here, which meant a CONTIGUOUS partition (used for
         * root, right after the ESP) could only ever mean "-1" (REST)
         * or an absolute LBA nobody at the call site could compute in
         * advance -- there was no way to give a CONTIGUOUS partition a
         * concrete size and leave the remainder of the disk free. */
        bool uses_computed_start = first_is_default || first_is_contiguous;
        uint64_t resolved_first;
        if (first_is_contiguous) {
            if (i == 0) { free(buf); return -EINVAL; }
            resolved_first = prev_end + 1;
        } else if (first_is_default) {
            resolved_first = first_usable;
        } else {
            resolved_first = p->first_lba;
        }

        uint64_t resolved_last;
        if (p->last_lba == KIBA_GPT_LAST_LBA_REST) {
            resolved_last = last_usable;
        } else if (uses_computed_start) {
            /* last_lba is a sector COUNT here, not an absolute LBA --
             * see the kiba_gpt_partition_t doc comment in kiba_gpt.h. */
            resolved_last = resolved_first + p->last_lba - 1;
        } else {
            resolved_last = p->last_lba;
        }

        snprintf(buf[i].n, sizeof(buf[i].n), "%zu:%llu:%llu", i + 1,
                  (unsigned long long)resolved_first, (unsigned long long)resolved_last);
        argv[ai++] = "-n"; argv[ai++] = buf[i].n;

        const char *type_code = (memcmp(p->type_guid.b, KIBA_GUID_ESP.b, 16) == 0)
                                     ? KIBA_TYPE_CODE_ESP : KIBA_TYPE_CODE_LINUX_FS;
        snprintf(buf[i].t, sizeof(buf[i].t), "%zu:%s", i + 1, type_code);
        argv[ai++] = "-t"; argv[ai++] = buf[i].t;

        if (p->name[0]) {
            snprintf(buf[i].c, sizeof(buf[i].c), "%zu:%s", i + 1, p->name);
            argv[ai++] = "-c"; argv[ai++] = buf[i].c;
        }
        if (!guid_is_zero(&p->unique_guid)) {
            char g[37];
            guid_to_str(&p->unique_guid, g);
            snprintf(buf[i].u, sizeof(buf[i].u), "%zu:%s", i + 1, g);
            argv[ai++] = "-u"; argv[ai++] = buf[i].u;
        }

        if (out_last_lba) out_last_lba[i] = resolved_last;
        prev_end = resolved_last;
    }

    argv[ai++] = dev_path;
    argv[ai]   = NULL;

    /* One sgdisk invocation writes the whole table (zap + optional disk
     * GUID + every partition) in a single pass, including the kernel
     * reread on success -- no separate partprobe step needed. */
    int rc = run_sgdisk(argv);
    free(buf);
    return rc;
}

/* ── Dual-boot: scan an existing table for an ESP + the largest gap ─── */
int kiba_gpt_scan(const char *path, kiba_gpt_scan_result_t *out) {
    memset(out, 0, sizeof(*out));

    uint32_t ssz = 0;
    uint64_t total_sectors = 0;
    if (kiba_gpt_probe_device(path, &ssz, &total_sectors) != 0) {
        /* Device couldn't even be opened/probed -- that's the one case
         * this function treats as a real error, per kiba_gpt.h. */
        return -errno;
    }

    char outbuf[16384];
    char *argv[] = { "sgdisk", "-p", (char *)path, NULL };
    int rc = run_sgdisk_capture(argv, outbuf, sizeof(outbuf));
    if (rc == -ENOENT) return rc; /* sgdisk itself missing -- real error */
    /* Any other non-zero exit (including "no valid partition table")
     * is treated as "nothing to scan" per kiba_gpt.h -- out stays
     * zeroed and this returns success, same as the old libfdisk version
     * did for a blank/non-GPT disk. */
    if (rc != 0) return 0;

    /* ── Parse "First usable sector is X, last usable sector is Y" ──── */
    uint64_t first_usable = 0, last_usable = 0;
    bool have_usable = false;
    {
        const char *m = strstr(outbuf, "First usable sector is ");
        if (m) {
            unsigned long long fu = 0, lu = 0;
            if (sscanf(m, "First usable sector is %llu, last usable sector is %llu",
                        &fu, &lu) == 2) {
                first_usable = fu; last_usable = lu; have_usable = true;
            }
        }
    }
    if (!have_usable) return 0; /* no valid GPT label found -- treat as empty */

    /* ── Parse the partition table lines: "  N   start   end   size  unit  code  name" ── */
    uint64_t starts[128], ends[128];
    size_t n = 0;
    char type_codes[128][8];

    char *line = outbuf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        unsigned partno = 0;
        unsigned long long s = 0, e = 0;
        char code[8] = {0};
        /* First three whitespace-separated tokens are partno/start/end;
         * skip the "size" + "unit" tokens, then grab the 4-hex-digit
         * type code. Any line that doesn't match this shape (headers,
         * blank lines, warnings) is simply not a partition row. */
        double size_val;
        char unit[8] = {0}, junk_size[16] = {0};
        int matched = sscanf(line, " %u %llu %llu %15s %7s %7s",
                              &partno, &s, &e, junk_size, unit, code);
        (void)size_val;
        if (matched == 6 && partno >= 1 && partno <= 128 && n < 128) {
            starts[n] = s;
            ends[n]   = e;
            snprintf(type_codes[n], sizeof(type_codes[n]), "%s", code);
            if (out->esp_partno == 0 && strcasecmp(code, KIBA_TYPE_CODE_ESP) == 0) {
                out->esp_partno    = (int)partno;
                out->esp_first_lba = s;
                out->esp_last_lba  = e;
            }
            n++;
        }

        line = nl ? nl + 1 : NULL;
    }

    /* Simple insertion sort by start LBA -- n is at most 128, and in
     * practice almost always under 10, so O(n^2) is irrelevant here. */
    for (size_t i = 1; i < n; i++) {
        uint64_t s = starts[i], e = ends[i];
        size_t j = i;
        while (j > 0 && starts[j - 1] > s) {
            starts[j] = starts[j - 1];
            ends[j]   = ends[j - 1];
            j--;
        }
        starts[j] = s;
        ends[j]   = e;
    }

    uint64_t best_first = 0, best_last = 0, best_len = 0;
    uint64_t cursor = first_usable;
    for (size_t i = 0; i <= n; i++) {
        uint64_t gap_start = cursor;
        uint64_t gap_end   = (i < n) ? (starts[i] > 0 ? starts[i] - 1 : 0)
                                      : last_usable;
        if (gap_end >= gap_start) {
            uint64_t glen = gap_end - gap_start + 1;
            if (glen > best_len) { best_len = glen; best_first = gap_start; best_last = gap_end; }
        }
        if (i < n) cursor = ends[i] + 1;
    }

    if (best_len > 0) {
        out->free_first_lba = best_first;
        out->free_last_lba  = best_last;
        out->free_bytes     = best_len * (uint64_t)(ssz ? ssz : 512);
    } else {
        /* No free space: make free_last_lba < free_first_lba so callers
         * can check "free_last_lba >= free_first_lba" as the has-room test. */
        out->free_first_lba = 1;
        out->free_last_lba  = 0;
    }
    return 0;
}

/* ── Dual-boot: append one partition to an existing table ───────────── */
int kiba_gpt_add_partition(const char *path, const kiba_gpt_partition_t *part,
                            int *out_partno) {
    /* Caller is responsible for having already confirmed (via
     * kiba_gpt_scan()) that a GPT label exists. sgdisk itself refuses
     * to add a sane partition to a disk with no label rather than
     * silently creating one -- that's what kiba_gpt_write() is for. */

    uint64_t resolved_last = part->last_lba;
    if (part->last_lba == KIBA_GPT_LAST_LBA_REST) {
        uint32_t ssz = 0;
        uint64_t total_sectors = 0;
        if (kiba_gpt_probe_device(path, &ssz, &total_sectors) != 0) return -errno;
        uint64_t entry_array_sectors = (16384 + ssz - 1) / ssz;
        resolved_last = total_sectors - 2 - entry_array_sectors;
    }

    /* "0" as the partition number tells sgdisk to use the next free
     * slot itself -- we don't need to know which slots are occupied. */
    char n_arg[48];
    snprintf(n_arg, sizeof(n_arg), "0:%llu:%llu",
              (unsigned long long)part->first_lba, (unsigned long long)resolved_last);

    const char *type_code = (memcmp(part->type_guid.b, KIBA_GUID_ESP.b, 16) == 0)
                                 ? KIBA_TYPE_CODE_ESP : KIBA_TYPE_CODE_LINUX_FS;
    char t_arg[24];
    snprintf(t_arg, sizeof(t_arg), "0:%s", type_code);

    char c_arg[80] = {0};
    if (part->name[0]) snprintf(c_arg, sizeof(c_arg), "0:%s", part->name);

    char u_arg[56] = {0};
    if (!guid_is_zero(&part->unique_guid)) {
        char g[37];
        guid_to_str(&part->unique_guid, g);
        snprintf(u_arg, sizeof(u_arg), "0:%s", g);
    }

    char *argv[14];
    size_t ai = 0;
    argv[ai++] = "sgdisk";
    argv[ai++] = "-a"; argv[ai++] = "1"; /* see kiba_gpt_write() -- keep our LBAs exact */
    argv[ai++] = "-n"; argv[ai++] = n_arg;
    argv[ai++] = "-t"; argv[ai++] = t_arg;
    if (c_arg[0]) { argv[ai++] = "-c"; argv[ai++] = c_arg; }
    if (u_arg[0]) { argv[ai++] = "-u"; argv[ai++] = u_arg; }
    argv[ai++] = (char *)path;
    argv[ai]   = NULL;

    int rc = run_sgdisk(argv);
    if (rc != 0) return rc;

    /* Find the partition number sgdisk actually assigned by re-scanning
     * the table and matching on the start LBA we just requested. */
    if (out_partno) {
        kiba_gpt_scan_result_t scan;
        if (kiba_gpt_scan(path, &scan) == 0) {
            char outbuf[16384];
            char *pargv[] = { "sgdisk", "-p", (char *)path, NULL };
            if (run_sgdisk_capture(pargv, outbuf, sizeof(outbuf)) == 0) {
                unsigned partno = 0;
                unsigned long long s = 0;
                char *line = outbuf;
                while (line && *line) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    unsigned long long e; char junk[16], unit[8], code[8];
                    if (sscanf(line, " %u %llu %llu %15s %7s %7s",
                                &partno, &s, &e, junk, unit, code) == 6 &&
                        s == part->first_lba) {
                        *out_partno = (int)partno;
                        break;
                    }
                    line = nl ? nl + 1 : NULL;
                }
            }
        }
    }
    return 0;
}

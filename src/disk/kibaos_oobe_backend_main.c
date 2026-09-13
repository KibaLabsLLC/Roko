/* kibaos_oobe_backend_main.c — the privileged install orchestrator.
 *
 * Invoked via sudo (no D-Bus/polkit dependency): `sudo /usr/local/bin/kibaos-oobe-backend
 * <disk> <mode> <locale> <keymap> <hostname> <username> <password>` — argv,
 * no shell, per the injection fix already applied on the Vala side.
 * <mode> is "erase" (wipe the whole disk, original behavior) or
 * "alongside" (dual-boot: keep whatever's already on the disk, reuse its
 * existing ESP, and install KibaOS into the largest free-space gap).
 * Windows app support (WinApps) is a listed, always-on feature, not a
 * user choice: this always drops /etc/kibaos/winapps-pending under
 * target_root so kibaos-winapps-firstrun.desktop (see WINDOWS APP SUPPORT
 * below) offers the WinApps setup wizard on first login into the freshly
 * installed system.
 *
 * Internally this no longer touches archinstall, parted, blkid, or
 * partprobe as subprocesses: all of that is libkibadisk (kiba_gpt.c /
 * kiba_fs.c / kiba_udev.c). The only external tools left are the ones
 * with no sane from-scratch replacement: sgdisk (GPT writer, see
 * kiba_gpt.c), unsquashfs, useradd/chpasswd, bootctl,
 * mkinitcpio, locale-gen, pacman -- all invoked via argv
 * arrays inside libkibadisk, never through a shell.
 *
 * Output protocol is unchanged on purpose: "PROGRESS <pct> <msg>" on
 * stdout, one line, matching what main.vala's read_backend_output()
 * already parses. Nothing on the Vala side needs to change.
 */
#define _GNU_SOURCE
#include "kiba_gpt.h"
#include "kiba_fs.h"
#include "kiba_udev.h"
#include "kiba_install.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/fs.h>   /* BLKRRPART */
#include <time.h>
#include <unistd.h>

static FILE *g_logfp = NULL;

static void log_init(void) {
    /* Open (create/append) the log file the UI tells the user to check.
     * Nothing upstream of this ever actually opened it before now — the
     * backend only wrote to stdout/stderr, which the frontend silently
     * dropped half of (see SubprocessLauncher fix in the Vala frontend:
     * it only piped STDOUT, so every FATAL: line on stderr went nowhere). */
    g_logfp = fopen("/var/log/kibaos-oobe.log", "a");
    if (g_logfp) {
        setvbuf(g_logfp, NULL, _IOLBF, 0); /* line-buffered: survives a crash/kill */
        time_t now = time(NULL);
        fprintf(g_logfp, "\n=== kibaos-oobe-backend started %s", ctime(&now));
        fflush(g_logfp);
    }
    /* Non-fatal if this fails (e.g. /var/log not writable yet at this point
     * in boot) — we still have stdout/stderr as a fallback, just no file. */
}

static void progress(int pct, const char *msg) {
    printf("PROGRESS %d %s\n", pct, msg);
    fflush(stdout);
    if (g_logfp) { fprintf(g_logfp, "PROGRESS %d %s\n", pct, msg); fflush(g_logfp); }
}

static void progress_cb(int pct, const char *msg, void *ud) {
    (void)ud;
    progress(pct, msg);
}

static void fail(const char *msg) {
    progress(100, msg);
    fprintf(stderr, "FATAL: %s\n", msg);
    if (g_logfp) { fprintf(g_logfp, "FATAL: %s\n", msg); fflush(g_logfp); fclose(g_logfp); }
    exit(1);
}

/* VM detection is no longer used to refuse installation here (see the
 * matching change in the Vala frontend's is_running_in_vm() comment) --
 * virtual disk handling has been solid enough in practice that the
 * original blanket refusal was pure friction, not a real safeguard. */

/* Builds the device path for partition number `n` of `disk` into `buf`.
 * Real rule (confirmed against ArchWiki's device-naming page): if the
 * disk's device name ends in a digit, partitions get a 'p' separator
 * (/dev/loop0p1, /dev/nvme0n1p1); otherwise they don't (/dev/vda1,
 * /dev/sda1). This is NOT about which driver/bus is involved (virtio vs
 * nvme vs scsi) -- it's purely about whether the trailing character of
 * the disk name is already a digit, which would otherwise make "loop01"
 * ambiguous (loop-1 vs loop0-partition-1). Checking for "nvme"/"mmcblk"
 * by substring was the previous (wrong) approach -- it happened to work
 * for /dev/vda by accident, but failed for /dev/loop0. */
static void partition_path(const char *disk, int n, char *buf, size_t buf_len) {
    size_t disk_len = strlen(disk);
    bool ends_in_digit = disk_len > 0 && disk[disk_len - 1] >= '0' && disk[disk_len - 1] <= '9';
    if (ends_in_digit) snprintf(buf, buf_len, "%sp%d", disk, n);
    else                snprintf(buf, buf_len, "%s%d", disk, n);
}

/* Force the kernel to re-read the partition table right now, rather
 * than relying on it to notice on its own before kiba_wait_for_device()
 * starts polling below. Direct ioctl instead of shelling out to
 * `blockdev --rereadpt`: run_argv() is a static helper private to the
 * libkibadisk translation units, not visible here, and this is a
 * one-line kernel call anyway -- no subprocess needed. */
static void kiba_force_reread_partition_table(const char *disk) {
    int fd = open(disk, O_RDONLY);
    if (fd < 0) return; /* best-effort */
    ioctl(fd, BLKRRPART, NULL); /* best-effort, ignore rc -- if this
                                  * fails, kiba_wait_for_device() below
                                  * will time out and surface it */
    close(fd);
}

/* Copies a single regular file, preserving permission bits. Best-effort:
 * returns -1 on any failure, but never calls fail()/exits -- GDM's
 * config is not load-bearing for the install, just for how the freshly
 * installed system happens to look on first boot. */
static int kiba_copy_file(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;

    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) return -1;

    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (out_fd < 0) { close(in_fd); return -1; }

    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, n - off);
            if (w < 0) { ok = 0; break; }
            off += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = 0;

    close(in_fd);
    close(out_fd);
    return ok ? 0 : -1;
}

/* Strips the live session's autologin out of a copied custom.conf.
 * The live ISO's GDM is configured (by the ISO build, not by anything
 * in this backend) to auto-login as "liveuser" so the live session boots
 * straight to a desktop with no login prompt -- exactly what you don't
 * want on the installed system, where "liveuser" doesn't even exist as
 * an account (kiba_install_create_user() above creates `username`, not
 * "liveuser") and where a login prompt is the whole point.
 *
 * custom.conf is a small GLib keyfile (INI-style: "[section]" headers,
 * "Key=Value" lines, "#"-prefixed comments). Rather than link against
 * GLib's keyfile parser for two keys, this does a line-oriented rewrite:
 * within the [daemon] section, any "AutomaticLoginEnable" line is
 * rewritten to "AutomaticLoginEnable=false" (rather than deleted --
 * GDM is happy with the key present-and-false, and leaving it in place
 * documents that autologin was considered and deliberately turned off,
 * instead of just silently absent) and any "AutomaticLogin" line
 * (the username to log in as) is dropped entirely, since a stale
 * "AutomaticLogin=liveuser" left behind next to
 * "AutomaticLoginEnable=false" is dead config pointing at a user that
 * no longer exists -- confusing for whoever reads this file later, and
 * one accidental "=true" edit away from GDM trying to log into an
 * account that isn't there. Whitespace-insensitive on both key and '='
 * (GDM itself trims around '='), so "AutomaticLogin = liveuser" is
 * matched too, not just the exact no-spaces form. Section tracking is
 * simple by design: custom.conf only ever has [daemon]/[security]/
 * [xdmcp]/[chooser]/[debug] as top-level sections with no nesting, so a
 * single "currently inside [daemon]?" flag is sufficient -- no general
 * INI-section stack needed. Best-effort and silent on any failure: a
 * missing or unreadable custom.conf just means this is a no-op, which
 * is fine since GDM defaults to no autologin when the key is absent
 * anyway; a missing GDM install (a non-GNOME KibaOS variant with a
 * different display manager) hits the same no-op path via
 * kiba_gdm_copy_and_disable_autologin()'s stat() check below. */
static void kiba_gdm_disable_autologin(const char *custom_conf_path) {
    FILE *in = fopen(custom_conf_path, "r");
    if (!in) return;

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.kibaos-tmp", custom_conf_path);
    FILE *out = fopen(tmp_path, "w");
    if (!out) { fclose(in); return; }

    char line[1024];
    bool in_daemon_section = false;
    while (fgets(line, sizeof(line), in)) {
        /* Work on a trimmed copy for matching, but preserve/rewrite the
         * real line (including its original line ending) for output. */
        char trimmed[1024];
        strncpy(trimmed, line, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';

        char *p = trimmed;
        while (isspace((unsigned char)*p)) p++;
        size_t tlen = strlen(p);
        while (tlen > 0 && isspace((unsigned char)p[tlen - 1])) p[--tlen] = '\0';

        if (p[0] == '[') {
            in_daemon_section = (strcmp(p, "[daemon]") == 0);
            fputs(line, out);
            continue;
        }

        if (in_daemon_section) {
            /* Split "Key = Value" / "Key=Value" on the first '=' and
             * trim the key side for comparison. */
            char *eq = strchr(p, '=');
            if (eq) {
                char key[128];
                size_t klen = (size_t)(eq - p);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, p, klen);
                key[klen] = '\0';
                size_t kt = strlen(key);
                while (kt > 0 && isspace((unsigned char)key[kt - 1])) key[--kt] = '\0';

                if (strcmp(key, "AutomaticLoginEnable") == 0) {
                    fputs("AutomaticLoginEnable=false\n", out);
                    continue;
                }
                if (strcmp(key, "AutomaticLogin") == 0) {
                    continue; /* drop the line entirely */
                }
            }
        }

        fputs(line, out);
    }

    fclose(in);
    fclose(out);
    rename(tmp_path, custom_conf_path); /* atomic swap over the original */
}

/* Copies the live session's GDM config onto the freshly installed root,
 * then immediately strips the live-session autologin out of the copy.
 *
 * "GDM's config tree" turned out not to be a tree at all: GDM has no
 * LightDM-style conf.d layering (confirmed both against the GDM package
 * itself and against this same build's own OEM-finish path, which has
 * to fall back to deleting custom.conf outright for exactly that reason
 * -- there's no higher-priority drop-in file it could leave instead).
 * /etc/gdm on a stock install holds exactly one file that matters:
 * custom.conf. The greeter branding (background, logo, dark mode) isn't
 * under /etc/gdm either -- it lives in the "gdm" dconf system db
 * (/etc/dconf/profile/gdm, /etc/dconf/db/gdm.d/*), which is static,
 * baked into the squashfs at ISO-build time, and already lands on
 * target_root for free as part of the full image extraction in step
 * 5-6 above. So a generic recursive directory copy here was solving a
 * problem that doesn't exist -- it just re-copied /etc/gdm/custom.conf
 * onto itself (target_root's copy, from image extraction, already had
 * identical content) while adding real failure modes of its own
 * (symlink traversal, unbounded recursion, fixed 1024-byte path
 * buffers) for a directory that never has anything in it to trip them.
 *
 * Copying the live system's custom.conf directly (rather than just
 * editing the one image extraction already placed on target_root) is
 * still worth doing though: it's the one part of /etc/gdm that plausibly
 * diverges between the squashfs and the running live session (GDM/the
 * live session's own runtime state living in the writable overlay, not
 * the read-only image), so this still copy-then-edits rather than just
 * edit-in-place -- copy first, then patch the copy under target_root,
 * never the live system's own /etc/gdm/custom.conf.
 *
 * Best-effort and non-fatal by design, same reasoning as the WinApps
 * marker below: a KibaOS variant/spin without GDM (a non-GNOME session)
 * simply has no /etc/gdm/custom.conf to copy, and that is a normal,
 * expected case, not an install failure. */
static void kiba_gdm_copy_and_disable_autologin(const char *target_root) {
    const char *src_conf = "/etc/gdm/custom.conf";
    struct stat st;
    if (stat(src_conf, &st) != 0 || !S_ISREG(st.st_mode)) {
        return; /* no GDM on this live medium/spin -- nothing to do */
    }

    char dst_gdm[320];
    snprintf(dst_gdm, sizeof(dst_gdm), "%s/etc/gdm", target_root);
    mkdir(dst_gdm, 0755); /* ignore EEXIST -- already present from image extraction */

    char dst_conf[352];
    snprintf(dst_conf, sizeof(dst_conf), "%s/custom.conf", dst_gdm);
    if (kiba_copy_file(src_conf, dst_conf) != 0) {
        return; /* best-effort, same as everything else in this function */
    }

    kiba_gdm_disable_autologin(dst_conf);
}

int main(int argc, char **argv) {
    log_init();
    if (argc != 8) {
        fprintf(stderr,
            "usage: %s <disk> <mode: erase|alongside> <locale> <keymap> <hostname> <username> <password>\n",
            argv[0]);
        return 2;
    }
    const char *disk     = argv[1];
    const char *mode     = argv[2];
    const char *locale   = argv[3];
    const char *keymap   = argv[4];
    const char *hostname = argv[5];
    const char *username = argv[6];
    const char *password = argv[7];

    bool dualboot = (strcmp(mode, "alongside") == 0);
    if (!dualboot && strcmp(mode, "erase") != 0) {
        fail("Unknown install mode (expected 'erase' or 'alongside').");
    }

    const char *target_root = "/mnt/kibaos-install";
    char esp_part[300], root_part[300];
    int esp_partno = 0, root_partno = 0;

    /* ── 1-2. Probe + partition (GPT via sgdisk) ───────────────────── */
    progress(2, "Reading disk information...");
    uint32_t ssz = 0;
    uint64_t total_sectors = 0;
    if (kiba_gpt_probe_device(disk, &ssz, &total_sectors) != 0) {
        fail("Could not read disk information.");
    }

    if (!dualboot) {
        /* ── Whole-disk install: wipe and lay down a fresh GPT ───────── */
        progress(6, "Partitioning disk...");
        int disk_fd = open(disk, O_RDWR);
        if (disk_fd < 0) fail("Could not open disk for writing.");

        /* Layout: 512MiB ESP (FAT32) + remainder as Linux root (ext4),
         * matching the layout the old archinstall-based backend used.
         *
         * Deliberately NOT re-deriving first/last usable LBAs here by
         * hand a second time. kiba_gpt_write() already computes them
         * once, internally, from the real sector size/total sectors and
         * the standard 128-entry GPT layout -- duplicating that math at
         * every call site is exactly how the old hand-rolled version
         * used to drift by a sector or two and get rejected. Just use
         * the sentinels below and let kiba_gpt_write() own the layout:
         * KIBA_GPT_FIRST_LBA_DEFAULT / _CONTIGUOUS / KIBA_GPT_LAST_LBA_REST
         * -- see kiba_gpt_write()'s handling of these sentinels and the
         * doc comment on kiba_gpt_partition_t. */
        uint64_t esp_sectors = (512ull * 1024 * 1024) / ssz;

        /* Rough pre-flight sanity check only (not used for the actual
         * partition layout below) -- catches "disk is way too small"
         * early with a friendly message instead of a raw sgdisk error. */
        uint64_t rough_overhead = (128 * 128 + ssz - 1) / ssz + 34;
        if (esp_sectors + rough_overhead >= total_sectors) {
            close(disk_fd);
            fail("Disk is too small for KibaOS (need at least ~1.5GB usable after the EFI partition).");
        }

        /* Root now gets everything left after the ESP -- the previous
         * "give root-a only half, leave the rest free for systemd-repart
         * to carve out root-b" A/B scheme has been removed entirely (see
         * the TRUE A/B ROOT section, which used to live further down in
         * this build script and no longer does). KIBA_GPT_LAST_LBA_REST
         * just fills the rest of the disk -- exactly the "disk too small
         * for two slots" fallback this code already had, now the only
         * path, so there's no root_sectors variable left to compute. */

        kiba_gpt_disk_t gdisk = {
            .fd = disk_fd,
            .logical_sector_size = ssz,
            .total_sectors = total_sectors,
            .disk_guid = {{0}},
        };
        kiba_gpt_partition_t parts[2] = {
            { .name = "KIBAOS-ESP",  .type_guid = KIBA_GUID_ESP,      .unique_guid = {{0}},
              .first_lba = KIBA_GPT_FIRST_LBA_DEFAULT, .last_lba = esp_sectors, .attributes = 0 },
            { .name = "KIBAOS-ROOT", .type_guid = KIBA_GUID_LINUX_FS, .unique_guid = {{0}},
              .first_lba = KIBA_GPT_FIRST_LBA_CONTIGUOUS,
              .last_lba = KIBA_GPT_LAST_LBA_REST, .attributes = 0 },
        };
        uint64_t placed_ends[2] = {0};
        int rc = kiba_gpt_write(&gdisk, parts, 2, placed_ends);
        close(disk_fd);
        if (rc != 0) {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "Partitioning failed: %s", strerror(-rc));
            fail(errbuf);
        }
        esp_partno = 1;
        root_partno = 2;
        partition_path(disk, esp_partno,  esp_part,  sizeof(esp_part));
        partition_path(disk, root_partno, root_part, sizeof(root_part));

        kiba_force_reread_partition_table(disk);
    } else {
        /* ── Dual-boot: reuse the existing ESP, use free space only ──── */
        progress(4, "Looking for an existing EFI partition and free space...");
        kiba_gpt_scan_result_t scan;
        if (kiba_gpt_scan(disk, &scan) != 0) {
            fail("Could not read the existing partition table.");
        }
        if (scan.esp_partno == 0) {
            fail("No existing EFI System Partition was found on this disk -- "
                 "install alongside needs one already present from the "
                 "other operating system.");
        }
        if (scan.free_last_lba < scan.free_first_lba) {
            fail("No usable free space was found on this disk to install "
                 "KibaOS alongside the existing operating system.");
        }
        const uint64_t min_root_bytes = 12ull * 1024 * 1024 * 1024; /* 12 GiB floor */
        if (scan.free_bytes < min_root_bytes) {
            fail("Not enough free space on this disk to install KibaOS "
                 "alongside the existing operating system (need at least ~12GB free).");
        }

        esp_partno = scan.esp_partno;
        partition_path(disk, esp_partno, esp_part, sizeof(esp_part));

        progress(6, "Creating KibaOS partition in free space...");
        kiba_gpt_partition_t root = {
            .name = "KIBAOS-ROOT", .type_guid = KIBA_GUID_LINUX_FS, .unique_guid = {{0}},
            .first_lba = scan.free_first_lba, .last_lba = scan.free_last_lba, .attributes = 0
        };
        int new_partno = 0;
        int rc = kiba_gpt_add_partition(disk, &root, &new_partno);
        if (rc != 0) {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "Partitioning failed: %s", strerror(-rc));
            fail(errbuf);
        }
        root_partno = new_partno;
        partition_path(disk, root_partno, root_part, sizeof(root_part));

        kiba_force_reread_partition_table(disk);
    }

    /* Wait for the kernel/udev to settle before touching the new
     * partition nodes -- the actual fix for the original bug report. */
    if (!kiba_wait_for_device(esp_part, 5000) || !kiba_wait_for_device(root_part, 5000)) {
        fail("Partition devices never appeared after partitioning.");
    }

    /* ── 3. Format ─────────────────────────────────────────────────── */
    progress(10, "Formatting partitions...");
    if (!dualboot) {
        if (kiba_fs_format(esp_part, KIBA_FS_FAT32, "KIBAOS-ESP") != 0) {
            fail(kiba_fs_strerror());
        }
        /* mkfs.fat just wrote a brand-new filesystem directly to the
         * block device -- the kernel/udev have no way to know that
         * happened on their own (see kiba_trigger_uevent's own comment
         * in kiba_udev.c for the full story: BLKPG at partition-create
         * time only covers the partition table, not what gets written
         * into a partition afterward). Without this, kiba_wait_for_
         * disk_tag() below could poll a /dev/disk/by-uuid symlink that
         * either never appears, or -- worse, and silently -- resolves to
         * a stale UUID left over from whatever was on this partition
         * before, which is exactly the kind of bug that produces a
         * clean-looking install that then can't find its own root
         * filesystem on first boot. */
        kiba_trigger_uevent(esp_part);
    }
    /* Dual-boot: the ESP already belongs to the other OS and already has
     * a filesystem on it, plus that OS's own boot files -- formatting it
     * would destroy them. bootctl install (further down) only ever adds
     * systemd-boot's own files there, so we deliberately never touch the ESP's
     * filesystem in this mode. */
    if (kiba_fs_format(root_part, KIBA_FS_EXT4, "KIBAOS-ROOT") != 0) {
        fail(kiba_fs_strerror());
    }
    kiba_trigger_uevent(root_part); /* same reasoning as the ESP one above */

    /* ── 4. Mount ──────────────────────────────────────────────────── */
    progress(14, "Mounting target filesystem...");
    mkdir(target_root, 0755);
    if (kiba_fs_mount(root_part, target_root, "ext4", NULL) != 0) fail(kiba_fs_strerror());
    char boot_dir[320];
    snprintf(boot_dir, sizeof(boot_dir), "%s/boot", target_root);
    mkdir(boot_dir, 0755);
    if (kiba_fs_mount(esp_part, boot_dir, "vfat", NULL) != 0) fail(kiba_fs_strerror());

    /* ── 5-6. Find + extract the live image onto the new root ───────── */
    char image_path[512];
    progress(18, "Locating KibaOS system image...");
    if (!kiba_find_live_image(image_path, sizeof(image_path))) {
        fail("Could not locate the KibaOS system image on the boot medium.");
    }
    if (kiba_install_extract_image(image_path, target_root, progress_cb, NULL) != 0) {
        fail(kiba_install_strerror());
    }

    /* mkarchiso strips vmlinuz-linux/initramfs-linux.img out of the
     * airootfs before building the image extracted above -- pull them
     * back in from the boot medium or the install has no kernel. */
    progress(70, "Copying kernel to target system...");
    if (kiba_install_copy_kernel(image_path, target_root) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 7. Write fstab, locale, hostname using the filesystem UUIDs
     *     mkfs.fat/mkfs.ext4 just generated ──────────────────────────── */
    progress(72, "Writing system configuration...");
    char root_uuid[64], esp_uuid[64];

    /* fstab uses the filesystem UUID (not the GPT PARTUUID), matching
     * the old backend's behavior. The actual UUID was generated by
     * mkfs.ext4/mkfs.fat during formatting above; we read it back via
     * udev's /dev/disk/by-uuid symlinks (systemd-udevd is always
     * running on the real install target, so this is reliable there
     * -- now that kiba_trigger_uevent() forces a fresh probe right
     * after each format call above. Without that trigger this could
     * previously read back a stale UUID left over from a partition's
     * PREVIOUS filesystem on a disk that had been installed to before,
     * since udev has no way to notice a raw mkfs write on its own --
     * see kiba_trigger_uevent's comment in kiba_udev.c for the full
     * story, and note this can't be exercised in a udev-less sandbox
     * even though it can't be exercised in a udev-less sandbox). */
    if (!kiba_wait_for_disk_tag(root_part, "by-uuid", root_uuid, sizeof(root_uuid), 8000)) {
        fail("Could not determine root filesystem UUID after formatting.");
    }
    if (!kiba_wait_for_disk_tag(esp_part, "by-uuid", esp_uuid, sizeof(esp_uuid), 8000)) {
        fail("Could not determine ESP filesystem UUID after formatting.");
    }

    if (kiba_install_write_configs(target_root, root_uuid, esp_uuid,
                                    hostname, locale, keymap) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 8. Bind mounts for chroot operations ────────────────────────── */
    progress(76, "Preparing system for configuration...");
    {
        char p[320];
        snprintf(p, sizeof(p), "%s/dev", target_root);  mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/proc", target_root); mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/sys", target_root);  mkdir(p, 0755);
    }

    progress(78, "Generating locale...");
    if (kiba_install_locale_gen(target_root) != 0) fail(kiba_install_strerror());

    /* ── 9. User account ───────────────────────────────────────────── */
    progress(82, "Creating your account...");
    if (kiba_install_create_user(target_root, username, password) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 10. Display manager configuration ───────────────────────────
     * Carries the live session's /etc/gdm over onto the installed
     * system, then strips out the live-only "auto-login as liveuser"
     * setting -- see kiba_gdm_copy_and_disable_autologin()'s own
     * comment for why this has to happen in that order (copy, then
     * edit the copy) and why it's best-effort/non-fatal (no GDM on this
     * spin is a normal case, not an install failure). */
    progress(84, "Configuring display manager...");
    kiba_gdm_copy_and_disable_autologin(target_root);

    /* ── 11. Bootloader, services, initramfs ─────────────────────────── */
    if (kiba_install_finalize(target_root, disk, root_part, root_uuid, root_partno, dualboot,
                               progress_cb, NULL) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 12. Windows app support ────────────────────────────────────────
     * Listed as a standing feature, not opt-in, so this always drops the
     * marker inside the freshly installed root -- the actual setup wizard
     * (kibaos-winapps-setup) only ever runs later, on first login into the
     * *installed* system via kibaos-winapps-firstrun.desktop, never from
     * in here. Mirrors the exact same marker kibaos-oem-finish.sh drops
     * for OEM-finish mode, just written under target_root instead of the
     * live root since this path is a fresh install, not an already-booted
     * system. */
    {
        char p[320];
        snprintf(p, sizeof(p), "%s/etc/kibaos", target_root);
        mkdir(p, 0755); /* ignore EEXIST -- /etc already exists under target_root */
        snprintf(p, sizeof(p), "%s/etc/kibaos/winapps-pending", target_root);
        FILE *f = fopen(p, "w");
        if (f) fclose(f); /* best-effort: a missed marker just means the
                            * user runs "Set Up Windows Workspace" from the
                            * app menu themselves instead of it prompting them */
    }

    progress(98, "Finishing up...");
    kiba_fs_umount(boot_dir);
    kiba_fs_umount(target_root);

    progress(100, "Done");
    return 0;
}

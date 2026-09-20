/* kibaos_oobe_backend_main.c — the privileged install orchestrator.
 *
 * Invoked via sudo (no D-Bus/polkit dependency): `sudo /usr/local/bin/kibaos-oobe-backend
 * <disk> <mode> <locale> <keymap> <hostname> <username> <password> <telemetry>`
 * argv, no shell, per the injection fix already applied on the Vala side.
 * <mode> is "erase" (wipe the whole disk) or "alongside" (dual-boot).
 * <telemetry> is "1" (user agreed to share hardware data) or "0" (declined).
 *
 * This backend installs KibaD as part of the normal install flow (step 13):
 * the unit is enabled via a wants symlink, and the consent state file is
 * written directly here as a plain JSON write -- no helper script needed.
 * KibaD's binary and service file arrive on target_root for free from the
 * image extraction in step 5-6, so there's nothing to copy separately.
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
    g_logfp = fopen("/var/log/kibaos-oobe.log", "a");
    if (g_logfp) {
        setvbuf(g_logfp, NULL, _IOLBF, 0);
        time_t now = time(NULL);
        fprintf(g_logfp, "\n=== kibaos-oobe-backend started %s", ctime(&now));
        fflush(g_logfp);
    }
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

static void partition_path(const char *disk, int n, char *buf, size_t buf_len) {
    size_t disk_len = strlen(disk);
    bool ends_in_digit = disk_len > 0 && disk[disk_len - 1] >= '0' && disk[disk_len - 1] <= '9';
    if (ends_in_digit) snprintf(buf, buf_len, "%sp%d", disk, n);
    else                snprintf(buf, buf_len, "%s%d", disk, n);
}

static void kiba_force_reread_partition_table(const char *disk) {
    int fd = open(disk, O_RDONLY);
    if (fd < 0) return;
    ioctl(fd, BLKRRPART, NULL);
    close(fd);
}

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
                    continue;
                }
            }
        }

        fputs(line, out);
    }

    fclose(in);
    fclose(out);
    rename(tmp_path, custom_conf_path);
}

static void kiba_gdm_copy_and_disable_autologin(const char *target_root) {
    const char *src_conf = "/etc/gdm/custom.conf";
    struct stat st;
    if (stat(src_conf, &st) != 0 || !S_ISREG(st.st_mode)) return;

    char dst_gdm[320];
    snprintf(dst_gdm, sizeof(dst_gdm), "%s/etc/gdm", target_root);
    mkdir(dst_gdm, 0755);

    char dst_conf[352];
    snprintf(dst_conf, sizeof(dst_conf), "%s/custom.conf", dst_gdm);
    if (kiba_copy_file(src_conf, dst_conf) != 0) return;

    kiba_gdm_disable_autologin(dst_conf);
}

static void kiba_seed_default_session(const char *target_root, const char *username) {
    char parent[300], dir[320], path[352];
    snprintf(parent, sizeof(parent), "%s/var/lib/AccountsService", target_root);
    snprintf(dir,    sizeof(dir),    "%s/users", parent);
    mkdir(parent, 0755);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s", dir, username);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "[User]\n"
        "Session=budgie-desktop\n"
        "XSession=budgie-desktop\n"
        "SystemAccount=false\n");
    fclose(f);
}

/* ── KibaD install ─────────────────────────────────────────────────────
 * kibad and its service file land on target_root for free as part of the
 * image extraction in step 5-6 (they're baked into the KibaOS squashfs).
 * All this step needs to do is enable the systemd unit by dropping a
 * wants symlink -- the same thing `systemctl enable kibad` would do
 * inside a chroot, without needing the chroot or systemctl.
 *
 * Best-effort and non-fatal: a KibaOS build that ships without kibad
 * (a minimal/headless variant) would have no service file to enable,
 * and that's fine -- consent.rs's fail-closed path handles that case. */
static void install_kibad(const char *target_root) {
    char sys_dir[300], wants_dir[340], svc_link[384];
    snprintf(sys_dir,   sizeof(sys_dir),   "%s/etc/systemd/system", target_root);
    snprintf(wants_dir, sizeof(wants_dir), "%s/multi-user.target.wants", sys_dir);
    snprintf(svc_link,  sizeof(svc_link),  "%s/kibad.service", wants_dir);

    mkdir(sys_dir,   0755); /* ignore EEXIST */
    mkdir(wants_dir, 0755); /* ignore EEXIST */

    /* Symlink target is the installed-system path resolved at boot time,
     * not the live-medium path. */
    symlink("/usr/lib/systemd/system/kibad.service", svc_link); /* ignore EEXIST */
}

/* ── Telemetry consent state file ──────────────────────────────────────
 * Writes /etc/kibad/telemetry-consent.state on the installed system in
 * the exact JSON shape consent.rs expects (version, agreed, scopes,
 * recorded_at). This backend runs as root via sudo, so the file is
 * automatically root-owned -- no chown call needed. chmod 0644 satisfies
 * consent.rs's "not group/world-writable" check.
 *
 * Best-effort: if the write fails (permissions issue mid-install, full
 * filesystem, anything else unusual), consent.rs's fail-closed default
 * kicks in and the daemon simply doesn't run. The user can re-run the
 * consent flow from Switchboard → Privacy on first boot. */
static void write_telemetry_consent(const char *target_root, bool agreed) {
    char dir_path[320], file_path[352];
    snprintf(dir_path,  sizeof(dir_path),  "%s/etc/kibad", target_root);
    snprintf(file_path, sizeof(file_path), "%s/telemetry-consent.state", dir_path);

    mkdir(dir_path, 0755); /* ignore EEXIST */

    FILE *f = fopen(file_path, "w");
    if (!f) return;

    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", utc);

    const char *b = agreed ? "true" : "false";
    fprintf(f,
        "{\n"
        "  \"version\": 1,\n"
        "  \"agreed\": %s,\n"
        "  \"scopes\": {\n"
        "    \"device_inventory\": %s\n"
        "  },\n"
        "  \"recorded_at\": \"%s\"\n"
        "}\n",
        b, b, ts);

    fclose(f);
    chmod(file_path, 0644);
}

int main(int argc, char **argv) {
    log_init();
    if (argc != 9) {
        fprintf(stderr,
            "usage: %s <disk> <mode: erase|alongside> <locale> <keymap>"
            " <hostname> <username> <password> <telemetry: 0|1>\n",
            argv[0]);
        return 2;
    }
    const char *disk      = argv[1];
    const char *mode      = argv[2];
    const char *locale    = argv[3];
    const char *keymap    = argv[4];
    const char *hostname  = argv[5];
    const char *username  = argv[6];
    const char *password  = argv[7];
    bool telemetry_agreed = (strcmp(argv[8], "1") == 0);

    bool dualboot = (strcmp(mode, "alongside") == 0);
    if (!dualboot && strcmp(mode, "erase") != 0) {
        fail("Unknown install mode (expected 'erase' or 'alongside').");
    }

    const char *target_root = "/mnt/kibaos-install";
    char esp_part[300], root_part[300];
    int esp_partno = 0, root_partno = 0;

    /* ── 1-2. Probe + partition ─────────────────────────────────────── */
    progress(2, "Reading disk information...");
    uint32_t ssz = 0;
    uint64_t total_sectors = 0;
    if (kiba_gpt_probe_device(disk, &ssz, &total_sectors) != 0) {
        fail("Could not read disk information.");
    }

    if (!dualboot) {
        progress(6, "Partitioning disk...");
        int disk_fd = open(disk, O_RDWR);
        if (disk_fd < 0) fail("Could not open disk for writing.");

        uint64_t esp_sectors = (512ull * 1024 * 1024) / ssz;
        uint64_t rough_overhead = (128 * 128 + ssz - 1) / ssz + 34;
        if (esp_sectors + rough_overhead >= total_sectors) {
            close(disk_fd);
            fail("Disk is too small for KibaOS (need at least ~1.5GB usable after the EFI partition).");
        }

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
        progress(4, "Looking for an existing EFI partition and free space...");
        kiba_gpt_scan_result_t scan;
        if (kiba_gpt_scan(disk, &scan) != 0) {
            fail("Could not read the existing partition table.");
        }
        if (scan.esp_partno == 0) {
            fail("No existing EFI System Partition was found on this disk — "
                 "install alongside needs one already present from the "
                 "other operating system.");
        }
        if (scan.free_last_lba < scan.free_first_lba) {
            fail("No usable free space was found on this disk to install "
                 "KibaOS alongside the existing operating system.");
        }
        const uint64_t min_root_bytes = 12ull * 1024 * 1024 * 1024;
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

    if (!kiba_wait_for_device(esp_part, 5000) || !kiba_wait_for_device(root_part, 5000)) {
        fail("Partition devices never appeared after partitioning.");
    }

    /* ── 3. Format ──────────────────────────────────────────────────── */
    progress(10, "Formatting partitions...");
    if (!dualboot) {
        if (kiba_fs_format(esp_part, KIBA_FS_FAT32, "KIBAOS-ESP") != 0) fail(kiba_fs_strerror());
        kiba_trigger_uevent(esp_part);
    }
    if (kiba_fs_format(root_part, KIBA_FS_EXT4, "KIBAOS-ROOT") != 0) fail(kiba_fs_strerror());
    kiba_trigger_uevent(root_part);

    /* ── 4. Mount ───────────────────────────────────────────────────── */
    progress(14, "Mounting target filesystem...");
    mkdir(target_root, 0755);
    if (kiba_fs_mount(root_part, target_root, "ext4", NULL) != 0) fail(kiba_fs_strerror());
    char boot_dir[320];
    snprintf(boot_dir, sizeof(boot_dir), "%s/boot", target_root);
    mkdir(boot_dir, 0755);
    if (kiba_fs_mount(esp_part, boot_dir, "vfat", NULL) != 0) fail(kiba_fs_strerror());

    /* ── 5-6. Find + extract live image ─────────────────────────────── */
    char image_path[512];
    progress(18, "Locating KibaOS system image...");
    if (!kiba_find_live_image(image_path, sizeof(image_path))) {
        fail("Could not locate the KibaOS system image on the boot medium.");
    }
    if (kiba_install_extract_image(image_path, target_root, progress_cb, NULL) != 0) {
        fail(kiba_install_strerror());
    }

    progress(70, "Copying kernel to target system...");
    if (kiba_install_copy_kernel(image_path, target_root) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 7. fstab, locale, hostname ─────────────────────────────────── */
    progress(72, "Writing system configuration...");
    char root_uuid[64], esp_uuid[64];
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

    /* ── 8. Bind mounts ─────────────────────────────────────────────── */
    progress(76, "Preparing system for configuration...");
    {
        char p[320];
        snprintf(p, sizeof(p), "%s/dev", target_root);  mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/proc", target_root); mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/sys", target_root);  mkdir(p, 0755);
    }

    progress(78, "Generating locale...");
    if (kiba_install_locale_gen(target_root) != 0) fail(kiba_install_strerror());

    /* ── 9. User account ────────────────────────────────────────────── */
    progress(82, "Creating your account...");
    if (kiba_install_create_user(target_root, username, password) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 10. Display manager ─────────────────────────────────────────── */
    progress(84, "Configuring display manager...");
    kiba_gdm_copy_and_disable_autologin(target_root);
    kiba_seed_default_session(target_root, username);

    /* ── 11. Bootloader, services, initramfs ─────────────────────────── */
    if (kiba_install_finalize(target_root, disk, root_part, root_uuid, root_partno, dualboot,
                               progress_cb, NULL) != 0) {
        fail(kiba_install_strerror());
    }

    /* ── 12. Windows app support ─────────────────────────────────────── */
    {
        char p[320];
        snprintf(p, sizeof(p), "%s/etc/kibaos", target_root);
        mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/etc/kibaos/winapps-pending", target_root);
        FILE *f = fopen(p, "w");
        if (f) fclose(f);
    }

    /* ── 13. KibaD: enable systemd unit + write telemetry consent ───────
     * install_kibad() drops the wants symlink so kibad starts on first
     * boot. write_telemetry_consent() writes the state file consent.rs
     * reads -- both are best-effort and non-fatal (see each function's
     * own comment for the fallback behavior on failure). */
    progress(98, "Installing KibaD...");
    install_kibad(target_root);
    write_telemetry_consent(target_root, telemetry_agreed);

    progress(99, "Finishing up...");
    kiba_fs_umount(boot_dir);
    kiba_fs_umount(target_root);

    progress(100, "Done");
    return 0;
}
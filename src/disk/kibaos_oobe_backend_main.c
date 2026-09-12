/* kibaos_oobe_backend_main.c — privileged KibaOS install orchestrator.
 *
 * Invoked via sudo:
 *   sudo /usr/local/bin/kibaos-oobe-backend
 *   <disk> <mode> <locale> <keymap> <hostname> <username> <password>
 *
 * <mode> is "erase" or "alongside".
 *
 * The locale argument is retained in the CLI for frontend compatibility,
 * but locale generation is no longer performed by the backend.
 *
 * All disk/filesystem operations are handled by libkibadisk. External
 * programs are invoked by that library through argv arrays rather than
 * shell commands.
 *
 * Output protocol:
 *   PROGRESS <pct> <msg>
 *
 * This matches main.vala's read_backend_output().
 */

#define _GNU_SOURCE

#include "kiba_gpt.h"
#include "kiba_fs.h"
#include "kiba_udev.h"
#include "kiba_install.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *g_logfp = NULL;

static void log_init(void)
{
    g_logfp = fopen("/var/log/kibaos-oobe.log", "a");

    if (g_logfp) {
        setvbuf(g_logfp, NULL, _IOLBF, 0);

        time_t now = time(NULL);
        fprintf(g_logfp, "\n=== kibaos-oobe-backend started %s", ctime(&now));
        fflush(g_logfp);
    }
}

static void progress(int pct, const char *msg)
{
    printf("PROGRESS %d %s\n", pct, msg);
    fflush(stdout);

    if (g_logfp) {
        fprintf(g_logfp, "PROGRESS %d %s\n", pct, msg);
        fflush(g_logfp);
    }
}

static void progress_cb(int pct, const char *msg, void *ud)
{
    (void)ud;
    progress(pct, msg);
}

static void fail(const char *msg)
{
    progress(100, msg);
    fprintf(stderr, "FATAL: %s\n", msg);

    if (g_logfp) {
        fprintf(g_logfp, "FATAL: %s\n", msg);
        fflush(g_logfp);
        fclose(g_logfp);
        g_logfp = NULL;
    }

    exit(1);
}

/*
 * Builds the device path for partition number n.
 *
 * Devices ending in a digit need the "p" separator:
 *   /dev/nvme0n1 -> /dev/nvme0n1p1
 *   /dev/mmcblk0 -> /dev/mmcblk0p1
 *   /dev/loop0   -> /dev/loop0p1
 *
 * Devices not ending in a digit do not:
 *   /dev/sda -> /dev/sda1
 *   /dev/vda -> /dev/vda1
 */
static void partition_path(const char *disk,
                           int n,
                           char *buf,
                           size_t buf_len)
{
    size_t disk_len = strlen(disk);

    bool ends_in_digit =
        disk_len > 0 &&
        disk[disk_len - 1] >= '0' &&
        disk[disk_len - 1] <= '9';

    if (ends_in_digit)
        snprintf(buf, buf_len, "%sp%d", disk, n);
    else
        snprintf(buf, buf_len, "%s%d", disk, n);
}

/*
 * Force the kernel to reread the partition table.
 *
 * This replaces the old:
 *   blockdev --rereadpt
 *
 * with a direct ioctl.
 */
static void kiba_force_reread_partition_table(const char *disk)
{
    int fd = open(disk, O_RDONLY);

    if (fd < 0)
        return;

    (void)ioctl(fd, BLKRRPART, NULL);
    close(fd);
}

/*
 * Bind-mounts /dev and /sys, and mounts a fresh /proc, into the target
 * root so that anything run inside a chroot()'d target_root later
 * (useradd here, plus whatever kiba_install_finalize() execs for
 * mkinitcpio/the bootloader, and previously locale-gen) has a working
 * /proc, /dev, /sys to run against.
 *
 * Before this, the "Prepare chroot environment" step only mkdir'd these
 * three directories -- they existed, but were always empty. glibc and
 * most coreutils/shadow-utils tools read /proc at startup (e.g. to
 * resolve their own /proc/self/exe, or /proc/mounts), so with nothing
 * mounted there, anything exec'd inside the chroot fails in ways that
 * look like "can't execute" rather than a clean, specific error --
 * which matches useradd (and, before it was removed, locale-gen) both
 * failing here with no more specific reason attached.
 */
static bool kiba_mount_chroot_special(const char *target_root)
{
    char dev_path[320];
    char proc_path[320];
    char sys_path[320];

    snprintf(dev_path, sizeof(dev_path), "%s/dev", target_root);
    snprintf(proc_path, sizeof(proc_path), "%s/proc", target_root);
    snprintf(sys_path, sizeof(sys_path), "%s/sys", target_root);

    if (mount("/dev", dev_path, NULL, MS_BIND | MS_REC, NULL) != 0)
        return false;

    if (mount("proc", proc_path, "proc", 0, NULL) != 0)
        return false;

    if (mount("/sys", sys_path, NULL, MS_BIND | MS_REC, NULL) != 0)
        return false;

    return true;
}

/*
 * Reverse of kiba_mount_chroot_special(). Uses MNT_DETACH (lazy
 * unmount) so a mount that's still momentarily busy -- e.g. a udev
 * worker with an open handle somewhere under /dev -- can't wedge the
 * very last step of the install.
 */
static void kiba_unmount_chroot_special(const char *target_root)
{
    char dev_path[320];
    char proc_path[320];
    char sys_path[320];

    snprintf(dev_path, sizeof(dev_path), "%s/dev", target_root);
    snprintf(proc_path, sizeof(proc_path), "%s/proc", target_root);
    snprintf(sys_path, sizeof(sys_path), "%s/sys", target_root);

    umount2(sys_path, MNT_DETACH);
    umount2(proc_path, MNT_DETACH);
    umount2(dev_path, MNT_DETACH);
}

int main(int argc, char **argv)
{
    log_init();

    /*
     * Keep the locale argument in the command-line interface for
     * compatibility with the existing Vala frontend.
     *
     * The backend intentionally does not use it anymore.
     */
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s <disk> <mode: erase|alongside> "
                "<locale> <keymap> <hostname> <username> <password>\n",
                argv[0]);
        return 2;
    }

    const char *disk     = argv[1];
    const char *mode     = argv[2];
    const char *keymap   = argv[4];
    const char *hostname = argv[5];
    const char *username = argv[6];
    const char *password = argv[7];

    /*
     * argv[3] is the legacy locale argument.
     *
     * It is deliberately ignored. Locale generation was removed from
     * the installer pipeline.
     */
    (void)argv[3];

    bool dualboot = strcmp(mode, "alongside") == 0;

    if (!dualboot && strcmp(mode, "erase") != 0) {
        fail("Unknown install mode (expected 'erase' or 'alongside').");
    }

    const char *target_root = "/mnt/kibaos-install";

    char esp_part[300];
    char root_part[300];

    int esp_partno = 0;
    int root_partno = 0;

    /* ────────────────────────────────────────────────────────────────
     * 1-2. Probe + partition
     * ──────────────────────────────────────────────────────────────── */

    progress(2, "Reading disk information...");

    uint32_t ssz = 0;
    uint64_t total_sectors = 0;

    if (kiba_gpt_probe_device(disk, &ssz, &total_sectors) != 0) {
        fail("Could not read disk information.");
    }

    if (!dualboot) {

        /* ── Whole-disk installation ──────────────────────────────── */

        progress(6, "Partitioning disk...");

        int disk_fd = open(disk, O_RDWR);

        if (disk_fd < 0) {
            fail("Could not open disk for writing.");
        }

        /*
         * Layout:
         *
         *   512 MiB EFI System Partition
         *   remaining disk = KibaOS root
         *
         * kiba_gpt_write() owns the actual GPT layout calculations.
         */
        uint64_t esp_sectors =
            (512ull * 1024ull * 1024ull) / ssz;

        /*
         * Rough pre-flight sanity check.
         */
        uint64_t rough_overhead =
            (128ull * 128ull + ssz - 1ull) / ssz + 34ull;

        if (esp_sectors + rough_overhead >= total_sectors) {
            close(disk_fd);

            fail("Disk is too small for KibaOS "
                 "(need at least ~1.5GB usable after the EFI partition).");
        }

        kiba_gpt_disk_t gdisk = {
            .fd = disk_fd,
            .logical_sector_size = ssz,
            .total_sectors = total_sectors,
            .disk_guid = {{0}},
        };

        kiba_gpt_partition_t parts[2] = {
            {
                .name = "KIBAOS-ESP",
                .type_guid = KIBA_GUID_ESP,
                .unique_guid = {{0}},
                .first_lba = KIBA_GPT_FIRST_LBA_DEFAULT,
                .last_lba = esp_sectors,
                .attributes = 0
            },
            {
                .name = "KIBAOS-ROOT",
                .type_guid = KIBA_GUID_LINUX_FS,
                .unique_guid = {{0}},
                .first_lba = KIBA_GPT_FIRST_LBA_CONTIGUOUS,
                .last_lba = KIBA_GPT_LAST_LBA_REST,
                .attributes = 0
            }
        };

        uint64_t placed_ends[2] = {0};

        int rc = kiba_gpt_write(
            &gdisk,
            parts,
            2,
            placed_ends
        );

        close(disk_fd);

        if (rc != 0) {
            char errbuf[256];

            snprintf(
                errbuf,
                sizeof(errbuf),
                "Partitioning failed: %s",
                strerror(-rc)
            );

            fail(errbuf);
        }

        esp_partno = 1;
        root_partno = 2;

        partition_path(
            disk,
            esp_partno,
            esp_part,
            sizeof(esp_part)
        );

        partition_path(
            disk,
            root_partno,
            root_part,
            sizeof(root_part)
        );

        kiba_force_reread_partition_table(disk);

    } else {

        /* ── Dual-boot installation ───────────────────────────────── */

        progress(
            4,
            "Looking for an existing EFI partition and free space..."
        );

        kiba_gpt_scan_result_t scan;

        if (kiba_gpt_scan(disk, &scan) != 0) {
            fail("Could not read the existing partition table.");
        }

        if (scan.esp_partno == 0) {
            fail(
                "No existing EFI System Partition was found on this disk -- "
                "install alongside needs one already present from the "
                "other operating system."
            );
        }

        if (scan.free_last_lba < scan.free_first_lba) {
            fail(
                "No usable free space was found on this disk to install "
                "KibaOS alongside the existing operating system."
            );
        }

        const uint64_t min_root_bytes =
            12ull * 1024ull * 1024ull * 1024ull;

        if (scan.free_bytes < min_root_bytes) {
            fail(
                "Not enough free space on this disk to install KibaOS "
                "alongside the existing operating system "
                "(need at least ~12GB free)."
            );
        }

        esp_partno = scan.esp_partno;

        partition_path(
            disk,
            esp_partno,
            esp_part,
            sizeof(esp_part)
        );

        progress(
            6,
            "Creating KibaOS partition in free space..."
        );

        kiba_gpt_partition_t root = {
            .name = "KIBAOS-ROOT",
            .type_guid = KIBA_GUID_LINUX_FS,
            .unique_guid = {{0}},
            .first_lba = scan.free_first_lba,
            .last_lba = scan.free_last_lba,
            .attributes = 0
        };

        int new_partno = 0;

        int rc = kiba_gpt_add_partition(
            disk,
            &root,
            &new_partno
        );

        if (rc != 0) {
            char errbuf[256];

            snprintf(
                errbuf,
                sizeof(errbuf),
                "Partitioning failed: %s",
                strerror(-rc)
            );

            fail(errbuf);
        }

        root_partno = new_partno;

        partition_path(
            disk,
            root_partno,
            root_part,
            sizeof(root_part)
        );

        kiba_force_reread_partition_table(disk);
    }

    /* ────────────────────────────────────────────────────────────────
     * Wait for partition devices
     * ──────────────────────────────────────────────────────────────── */

    if (!kiba_wait_for_device(esp_part, 5000) ||
        !kiba_wait_for_device(root_part, 5000)) {

        fail("Partition devices never appeared after partitioning.");
    }

    /* ────────────────────────────────────────────────────────────────
     * 3. Format
     * ──────────────────────────────────────────────────────────────── */

    progress(10, "Formatting partitions...");

    if (!dualboot) {

        if (kiba_fs_format(
                esp_part,
                KIBA_FS_FAT32,
                "KIBAOS-ESP") != 0) {

            fail(kiba_fs_strerror());
        }

        /*
         * Force udev to notice the newly-created filesystem.
         */
        kiba_trigger_uevent(esp_part);
    }

    /*
     * In alongside mode the existing ESP is intentionally never
     * formatted. Its existing boot files must remain intact.
     */
    if (kiba_fs_format(
            root_part,
            KIBA_FS_EXT4,
            "KIBAOS-ROOT") != 0) {

        fail(kiba_fs_strerror());
    }

    kiba_trigger_uevent(root_part);

    /* ────────────────────────────────────────────────────────────────
     * 4. Mount
     * ──────────────────────────────────────────────────────────────── */

    progress(14, "Mounting target filesystem...");

    (void)mkdir(target_root, 0755);

    if (kiba_fs_mount(
            root_part,
            target_root,
            "ext4",
            NULL) != 0) {

        fail(kiba_fs_strerror());
    }

    char boot_dir[320];

    snprintf(
        boot_dir,
        sizeof(boot_dir),
        "%s/boot",
        target_root
    );

    (void)mkdir(boot_dir, 0755);

    if (kiba_fs_mount(
            esp_part,
            boot_dir,
            "vfat",
            NULL) != 0) {

        fail(kiba_fs_strerror());
    }

    /* ────────────────────────────────────────────────────────────────
     * 5-6. Locate + extract system image
     * ──────────────────────────────────────────────────────────────── */

    char image_path[512];

    progress(
        18,
        "Locating KibaOS system image..."
    );

    if (!kiba_find_live_image(
            image_path,
            sizeof(image_path))) {

        fail(
            "Could not locate the KibaOS system image "
            "on the boot medium."
        );
    }

    if (kiba_install_extract_image(
            image_path,
            target_root,
            progress_cb,
            NULL) != 0) {

        fail(kiba_install_strerror());
    }

    /*
     * mkarchiso removes the kernel and initramfs from the airootfs
     * before producing the compressed system image.
     */
    progress(
        70,
        "Copying kernel to target system..."
    );

    if (kiba_install_copy_kernel(
            image_path,
            target_root) != 0) {

        fail(kiba_install_strerror());
    }

    /* ────────────────────────────────────────────────────────────────
     * 7. Filesystem/system configuration
     * ──────────────────────────────────────────────────────────────── */

    progress(
        72,
        "Writing system configuration..."
    );

    char root_uuid[64];
    char esp_uuid[64];

    /*
     * Obtain filesystem UUIDs generated by mkfs.
     */
    if (!kiba_wait_for_disk_tag(
            root_part,
            "by-uuid",
            root_uuid,
            sizeof(root_uuid),
            8000)) {

        fail(
            "Could not determine root filesystem UUID "
            "after formatting."
        );
    }

    if (!kiba_wait_for_disk_tag(
            esp_part,
            "by-uuid",
            esp_uuid,
            sizeof(esp_uuid),
            8000)) {

        fail(
            "Could not determine ESP filesystem UUID "
            "after formatting."
        );
    }

    /*
     * Locale is intentionally absent here.
     *
     * kiba_install_write_configs() now writes:
     *   /etc/fstab
     *   /etc/hostname
     *   /etc/hosts
     *   /etc/vconsole.conf
     *
     * It does not generate locales or write locale.conf.
     */
    if (kiba_install_write_configs(
            target_root,
            root_uuid,
            esp_uuid,
            hostname,
            keymap) != 0) {

        fail(kiba_install_strerror());
    }

    /* ────────────────────────────────────────────────────────────────
     * 8. Prepare chroot environment
     * ──────────────────────────────────────────────────────────────── */

    progress(
        76,
        "Preparing system for configuration..."
    );

    {
        char p[320];

        snprintf(
            p,
            sizeof(p),
            "%s/dev",
            target_root
        );
        (void)mkdir(p, 0755);

        snprintf(
            p,
            sizeof(p),
            "%s/proc",
            target_root
        );
        (void)mkdir(p, 0755);

        snprintf(
            p,
            sizeof(p),
            "%s/sys",
            target_root
        );
        (void)mkdir(p, 0755);

        if (!kiba_mount_chroot_special(target_root)) {
            fail(
                "Could not mount /dev, /proc, or /sys into the "
                "target system."
            );
        }
    }

    /*
     * Locale generation was intentionally removed.
     *
     * There is no kiba_install_locale_gen() call anymore.
     */

    /* ────────────────────────────────────────────────────────────────
     * 9. User account
     * ──────────────────────────────────────────────────────────────── */

    progress(
        82,
        "Creating your account..."
    );

    if (kiba_install_create_user(
            target_root,
            username,
            password) != 0) {

        fail(kiba_install_strerror());
    }

    /* ────────────────────────────────────────────────────────────────
     * 10. Bootloader, services and initramfs
     * ──────────────────────────────────────────────────────────────── */

    if (kiba_install_finalize(
            target_root,
            disk,
            root_part,
            root_uuid,
            root_partno,
            dualboot,
            progress_cb,
            NULL) != 0) {

        fail(kiba_install_strerror());
    }

    /* ────────────────────────────────────────────────────────────────
     * 11. Windows app support
     * ──────────────────────────────────────────────────────────────── */

    {
        char p[320];

        snprintf(
            p,
            sizeof(p),
            "%s/etc/kibaos",
            target_root
        );

        /*
         * /etc exists in the extracted system. Create the KibaOS
         * directory if it isn't already there.
         */
        (void)mkdir(p, 0755);

        snprintf(
            p,
            sizeof(p),
            "%s/etc/kibaos/winapps-pending",
            target_root
        );

        FILE *f = fopen(p, "w");

        if (f)
            fclose(f);
    }

    /* ────────────────────────────────────────────────────────────────
     * Finish
     * ──────────────────────────────────────────────────────────────── */

    progress(
        98,
        "Finishing up..."
    );

    kiba_unmount_chroot_special(target_root);
    kiba_fs_umount(boot_dir);
    kiba_fs_umount(target_root);

    progress(
        100,
        "Done"
    );

    if (g_logfp) {
        fclose(g_logfp);
        g_logfp = NULL;
    }

    return 0;
}

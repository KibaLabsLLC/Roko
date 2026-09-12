/* kiba_install_extract.c — squashfs/erofs location and extraction.
 * See kiba_install.h for design rationale. */
#define _GNU_SOURCE
#include "kiba_install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

char kiba_install_shared_err[256] = "no error";
const char *kiba_install_strerror(void) { return kiba_install_shared_err; }
#define g_install_err kiba_install_shared_err

static int run_argv(char *const argv[]) {
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "failed to spawn %s: %s", argv[0], strerror(rc));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "waitpid failed for %s: %s", argv[0], strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (WIFEXITED(status)) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "%s exited with status %d", argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "%s killed by signal %d", argv[0], WTERMSIG(status));
    } else {
        snprintf(g_install_err, sizeof(g_install_err), "%s terminated abnormally", argv[0]);
    }
    return -1;
}

/* Run argv with status >=0 considered acceptable up to max_nonfatal
 * (mirrors the old backend's tolerance of unsquashfs's non-fatal
 * warning exit codes, where only exit code 1 is truly fatal). */
static int run_argv_tolerant(char *const argv[], int max_nonfatal_exit) {
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "failed to spawn %s: %s", argv[0], strerror(rc));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "waitpid failed for %s: %s", argv[0], strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code <= max_nonfatal_exit) return code; /* 0 or tolerated warning code */
        snprintf(g_install_err, sizeof(g_install_err),
                  "%s exited with fatal status %d", argv[0], code);
        return -1;
    }
    snprintf(g_install_err, sizeof(g_install_err), "%s terminated abnormally", argv[0]);
    return -1;
}

/* Finds a directory containing one of these filenames anywhere below
 * `root`, by walking the tree ourselves (no `find` subprocess). Bounded
 * depth to avoid pathological scans of huge trees. */
static bool scan_dir_for_image(const char *root, char *out_path, size_t out_len, int max_depth) {
    static const char *names[] = { "airootfs.sfs", "airootfs.erofs" };

    DIR *d = opendir(root);
    if (!d) return false;

    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", root, ent->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {
            for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
                if (strcmp(ent->d_name, names[i]) == 0) {
                    snprintf(out_path, out_len, "%s", full);
                    found = true;
                    break;
                }
            }
        } else if (S_ISDIR(st.st_mode) && max_depth > 0) {
            if (scan_dir_for_image(full, out_path, out_len, max_depth - 1)) found = true;
        }
    }
    closedir(d);
    return found;
}

/* Parses /proc/self/mountinfo to find the mount target for the given
 * mount source's mountpoint matching `target_substr` in its mount
 * point path -- specifically, we want wherever /run/archiso/bootmnt
 * (or equivalent) is mounted. Avoids shelling out to `findmnt`. */
static bool mountinfo_find_target(const char *target_path, char *out_target, size_t out_len) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return false;

    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        /* mountinfo format: id parentid major:minor root mountpoint ...
         * Field 5 (1-indexed) is the mount point, space-delimited,
         * with systemd-style octal escapes we don't need to decode
         * for an exact-path comparison against /run/archiso/bootmnt. */
        char *fields[16] = {0};
        int n = 0;
        char *tok = strtok(line, " ");
        while (tok && n < 16) { fields[n++] = tok; tok = strtok(NULL, " "); }
        if (n < 5) continue;
        if (strcmp(fields[4], target_path) == 0) {
            snprintf(out_target, out_len, "%s", target_path);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool kiba_find_live_image(char *out_path, size_t out_len) {
    /* (a) ask the kernel (via mountinfo, not findmnt) where the boot
     * medium is actually mounted right now. */
    char bootmnt[256];
    if (mountinfo_find_target("/run/archiso/bootmnt", bootmnt, sizeof(bootmnt))) {
        if (scan_dir_for_image(bootmnt, out_path, out_len, 6)) return true;
    }

    /* (b) conventional archiso layout, direct check.
     * this binary only ever boots on the arch it was built for, so just
     * ask the kernel what we are instead of hardcoding it, chirp */
    struct utsname uts;
    const char *live_arch = "x86_64";
    if (uname(&uts) == 0 && uts.machine[0] != '\0') {
        live_arch = uts.machine; /* "x86_64" or "aarch64", straight from the kernel's mouth */
    }
    char conventional[5][256];
    snprintf(conventional[0], sizeof(conventional[0]), "/run/archiso/bootmnt/arch/%s/airootfs.sfs", live_arch);
    snprintf(conventional[1], sizeof(conventional[1]), "/run/archiso/bootmnt/arch/%s/airootfs.erofs", live_arch);
    snprintf(conventional[2], sizeof(conventional[2]), "/run/archiso/copytoram/arch/%s/airootfs.sfs", live_arch);
    snprintf(conventional[3], sizeof(conventional[3]), "/run/archiso/copytoram/arch/%s/airootfs.erofs", live_arch);
    snprintf(conventional[4], sizeof(conventional[4]), "/run/mnt/arch/%s/airootfs.sfs", live_arch);
    for (size_t i = 0; i < 5; i++) {
        struct stat st;
        if (stat(conventional[i], &st) == 0) {
            snprintf(out_path, out_len, "%s", conventional[i]);
            return true;
        }
    }

    /* (c) last resort: scan /run and /mnt entirely. */
    if (scan_dir_for_image("/run", out_path, out_len, 8)) return true;
    if (scan_dir_for_image("/mnt", out_path, out_len, 8)) return true;

    return false;
}

int kiba_install_extract_image(const char *image_path, const char *target_root,
                                kiba_progress_cb cb, void *user_data) {
    if (cb) cb(22, "Copying KibaOS to your computer (this takes a few minutes)...", user_data);

    size_t len = strlen(image_path);
    bool is_squashfs = (len >= 4 && strcmp(image_path + len - 4, ".sfs") == 0);

    if (is_squashfs) {
        char *argv[] = {
            (char *)"unsquashfs", (char *)"-f", (char *)"-d", (char *)target_root,
            (char *)"-no-progress", (char *)image_path, NULL
        };
        /* Exit code 1 is fatal; anything else (e.g. >1 for non-fatal
         * extraction warnings) is tolerated, matching the old backend. */
        int rc = run_argv_tolerant(argv, 255);
        if (rc < 0) return -1;
        if (rc == 1) { snprintf(g_install_err, sizeof(g_install_err), "unsquashfs fatal error"); return -1; }
        return 0;
    } else {
        /* EROFS: no in-place extractor; mount read-only via a loop
         * device and recursively copy. We use the `cp -a` binary here
         * deliberately rather than hand-rolling a recursive copy that
         * preserves xattrs/ACLs/special files/hardlinks/sparse files
         * correctly -- that's a much larger correctness surface than
         * mkfs, and cp is a stable, single-purpose coreutils tool. */
        char tmp_mnt[] = "/tmp/kiba-erofs-XXXXXX";
        if (!mkdtemp(tmp_mnt)) {
            snprintf(g_install_err, sizeof(g_install_err), "mkdtemp failed: %s", strerror(errno));
            return -1;
        }
        if (mount(image_path, tmp_mnt, "erofs", MS_RDONLY, "loop") != 0) {
            snprintf(g_install_err, sizeof(g_install_err), "erofs mount failed: %s", strerror(errno));
            rmdir(tmp_mnt);
            return -1;
        }
        char *argv[] = { (char *)"cp", (char *)"-a", (char *)"-T", tmp_mnt, (char *)target_root, NULL };
        int rc = run_argv(argv);
        umount2(tmp_mnt, 0);
        rmdir(tmp_mnt);
        return rc;
    }
}

int kiba_install_copy_kernel(const char *image_path, const char *target_root) {
    /* image_path is ".../<install_dir>/x86_64/airootfs.sfs" (or
     * airootfs.erofs). The kernel/initramfs mkarchiso stripped out of
     * the image live at the sibling "<install_dir>/boot/x86_64/" dir
     * instead (see kiba_install.h for why). Peel off two path
     * components to get from the image to <install_dir>. */
    char work[512];
    int n = snprintf(work, sizeof(work), "%s", image_path);
    if (n <= 0 || (size_t)n >= sizeof(work)) {
        snprintf(g_install_err, sizeof(g_install_err), "image path too long");
        return -1;
    }

    char *slash = strrchr(work, '/');   /* strip "/airootfs.sfs" */
    if (!slash) {
        snprintf(g_install_err, sizeof(g_install_err), "unexpected image path layout: %s", image_path);
        return -1;
    }
    *slash = '\0';

    slash = strrchr(work, '/');         /* strip "/x86_64" */
    if (!slash) {
        snprintf(g_install_err, sizeof(g_install_err), "unexpected image path layout: %s", image_path);
        return -1;
    }
    *slash = '\0';
    /* work is now ".../<install_dir>" */

    /* same trick as kiba_find_live_image -- ask uname instead of guessing.
     * dst names stay generic (vmlinuz-linux) no matter the arch, so the
     * installed system's bootloader never has to know what booted it. */
    struct utsname kuts;
    const char *karch = "x86_64";
    const char *kpkg_suffix = "linux";           /* package name: "linux" or "linux-aarch64" */
    if (uname(&kuts) == 0 && kuts.machine[0] != '\0') {
        karch = kuts.machine;
        if (strcmp(karch, "aarch64") == 0) kpkg_suffix = "linux-aarch64";
    }
    char vmlinuz_src[600], initrd_src[600], vmlinuz_dst[600], initrd_dst[600];
    snprintf(vmlinuz_src, sizeof(vmlinuz_src), "%s/boot/%s/vmlinuz-%s", work, karch, kpkg_suffix);
    snprintf(initrd_src,  sizeof(initrd_src),  "%s/boot/%s/initramfs-%s.img", work, karch, kpkg_suffix);
    snprintf(vmlinuz_dst, sizeof(vmlinuz_dst), "%s/boot/vmlinuz-linux", target_root);
    snprintf(initrd_dst,  sizeof(initrd_dst),  "%s/boot/initramfs-linux.img", target_root);

    struct stat st;
    if (stat(vmlinuz_src, &st) != 0) {
        snprintf(g_install_err, sizeof(g_install_err),
                  "kernel not found on boot medium at %s: %s", vmlinuz_src, strerror(errno));
        return -1;
    }

    char *argv[] = { (char *)"cp", (char *)"-a", vmlinuz_src, vmlinuz_dst, NULL };
    if (run_argv(argv) != 0) return -1;

    /* initramfs-linux.img gets rebuilt from scratch a few steps later
     * in kiba_install_finalize (mkinitcpio -g), so this copy isn't load-
     * bearing the way vmlinuz-linux is -- but it means the target isn't
     * momentarily without any initrd at all if that later step fails
     * partway through, so still worth doing and still best-effort. */
    char *argv2[] = { (char *)"cp", (char *)"-a", initrd_src, initrd_dst, NULL };
    run_argv(argv2);

    return 0;
}

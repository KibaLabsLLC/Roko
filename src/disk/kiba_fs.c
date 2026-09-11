/* kiba_fs.c — see kiba_fs.h for scope/rationale. */
#define _GNU_SOURCE
#include "kiba_fs.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char g_last_error[256] = "no error";

const char *kiba_fs_strerror(void) { return g_last_error; }

/* Runs argv[0] with argv (NULL-terminated), via posix_spawn — never
 * touches /bin/sh, so there is no quoting/injection surface at all:
 * each element of argv is passed to execve() as a discrete argument
 * regardless of its contents (spaces, quotes, anything). Captures
 * only the exit code; does not parse the child's stdout/stderr for
 * control flow (we only care whether it succeeded). */
static int run_argv(char *const argv[]) {
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "failed to spawn %s: %s", argv[0], strerror(rc));
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "waitpid failed for %s: %s", argv[0], strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    if (WIFEXITED(status)) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "%s exited with status %d", argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "%s killed by signal %d", argv[0], WTERMSIG(status));
    } else {
        snprintf(g_last_error, sizeof(g_last_error),
                  "%s terminated abnormally", argv[0]);
    }
    return -1;
}

int kiba_fs_format(const char *part_path, kiba_fs_type_t type,
                    const char *volume_label) {
    if (!part_path) { snprintf(g_last_error, sizeof(g_last_error), "no partition path"); return -1; }

    if (type == KIBA_FS_FAT32) {
        /* mkfs.fat -F 32 [-n LABEL] <part> */
        char *argv[8];
        int i = 0;
        argv[i++] = (char *)"mkfs.fat";
        argv[i++] = (char *)"-F";
        argv[i++] = (char *)"32";
        if (volume_label) { argv[i++] = (char *)"-n"; argv[i++] = (char *)volume_label; }
        argv[i++] = (char *)part_path;
        argv[i++] = NULL;
        return run_argv(argv);
    } else if (type == KIBA_FS_EXT4) {
        /* mkfs.ext4 -F -q [-L LABEL] <part>
         * -F: force (skip the "are you sure" prompt — we already
         *     confirmed disk selection in the UI before reaching here)
         * -q: quiet (we don't parse its stdout regardless) */
        char *argv[8];
        int i = 0;
        argv[i++] = (char *)"mkfs.ext4";
        argv[i++] = (char *)"-F";
        /* TEMP: -q dropped for one test run so mkfs's actual stderr
         * message shows up instead of just "exited with status 1".
         * Put -q back once the real failure reason is known. */
        if (volume_label) { argv[i++] = (char *)"-L"; argv[i++] = (char *)volume_label; }
        argv[i++] = (char *)part_path;
        argv[i++] = NULL;
        return run_argv(argv);
    }

    snprintf(g_last_error, sizeof(g_last_error), "unknown filesystem type");
    return -1;
}

int kiba_fs_mount(const char *part_path, const char *target_dir,
                   const char *fstype, const char *options) {
    /* Direct mount(2) syscall — no `mount` binary, no shell. */
    unsigned long flags = 0;
    if (mount(part_path, target_dir, fstype, flags, options) != 0) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "mount(%s -> %s, %s) failed: %s",
                  part_path, target_dir, fstype, strerror(errno));
        return -1;
    }
    return 0;
}

int kiba_fs_umount(const char *target_dir) {
    if (umount2(target_dir, 0) != 0) {
        snprintf(g_last_error, sizeof(g_last_error),
                  "umount(%s) failed: %s", target_dir, strerror(errno));
        return -1;
    }
    return 0;
}

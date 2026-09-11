/* kiba_install_finish.c — configs, user account, bootloader, finalize. */
#define _GNU_SOURCE
#include "kiba_install.h"
#include "kiba_udev.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern char kiba_install_shared_err[256];
#define g_finish_err kiba_install_shared_err

static int run_argv(char *const argv[])
{
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "failed to spawn %s: %s", argv[0], strerror(rc));
        return -1;
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "waitpid failed for %s: %s", argv[0], strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    if (WIFEXITED(status))
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "%s exited with status %d", argv[0], WEXITSTATUS(status));
    else
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "%s terminated abnormally", argv[0]);

    return -1;
}

static int run_argv_with_stdin(char *const argv[], const char *data)
{
    int p[2];
    if (pipe(p) != 0) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "pipe() failed: %s", strerror(errno));
        return -1;
    }

    posix_spawn_file_actions_t a;
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_adddup2(&a, p[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&a, p[1]);

    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &a, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&a);
    close(p[0]);

    if (rc) {
        close(p[1]);
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "failed to spawn %s: %s", argv[0], strerror(rc));
        return -1;
    }

    size_t len = strlen(data), done = 0;
    while (done < len) {
        ssize_t n = write(p[1], data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        done += (size_t)n;
    }
    close(p[1]);

    int status;
    if (waitpid(pid, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "%s failed", argv[0]);
        return -1;
    }

    return 0;
}

static int chroot_run(const char *root, char *const inner[])
{
    char *argv[16];
    int i = 0;

    argv[i++] = (char *)"arch-chroot";
    argv[i++] = (char *)root;

    for (int j = 0; inner[j] && i < 15; j++)
        argv[i++] = inner[j];

    argv[i] = NULL;
    return run_argv(argv);
}

static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    size_t len = strlen(content), done = 0;
    while (done < len) {
        ssize_t n = write(fd, content + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "write(%s) failed: %s", path, strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }

    close(fd);
    return 0;
}

static bool file_contains_user(const char *path, const char *username)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512], prefix[256];
    snprintf(prefix, sizeof(prefix), "%s:", username);
    size_t len = strlen(prefix);
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, len) == 0) {
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

static bool group_exists(const char *target_root, const char *group)
{
    char path[1024], prefix[256], line[512];

    snprintf(path, sizeof(path), "%s/etc/group", target_root);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    snprintf(prefix, sizeof(prefix), "%s:", group);
    size_t len = strlen(prefix);
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, len) == 0) {
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

int kiba_install_write_configs(const char *target_root,
                               const char *root_uuid,
                               const char *esp_uuid,
                               const char *hostname,
                               const char *keymap)
{
    char path[1024], content[2048];

    snprintf(path, sizeof(path), "%s/etc/fstab", target_root);
    snprintf(content, sizeof(content),
             "# KibaOS fstab — generated by installer\n"
             "UUID=%s  /      ext4  defaults,noatime  0 1\n"
             "UUID=%s  /boot  vfat  umask=0077        0 2\n",
             root_uuid, esp_uuid);
    if (write_file(path, content)) return -1;

    snprintf(path, sizeof(path), "%s/etc/hostname", target_root);
    snprintf(content, sizeof(content), "%s\n", hostname);
    if (write_file(path, content)) return -1;

    snprintf(path, sizeof(path), "%s/etc/hosts", target_root);
    snprintf(content, sizeof(content),
             "127.0.0.1   localhost\n"
             "::1         localhost\n"
             "127.0.1.1   %s.localdomain %s\n",
             hostname, hostname);
    if (write_file(path, content)) return -1;

    /*
     * locale-gen intentionally removed.
     * The installed image is expected to already contain the required
     * locale data. locale.conf can be managed elsewhere.
     */

    snprintf(path, sizeof(path), "%s/etc/vconsole.conf", target_root);
    snprintf(content, sizeof(content), "KEYMAP=%s\n", keymap);
    if (write_file(path, content)) return -1;

    return 0;
}

int kiba_install_create_user(const char *target_root,
                             const char *username,
                             const char *password)
{
    char path[1024], content[512];

    /* Disable live-session autologin while preserving GDM configuration. */
    snprintf(path, sizeof(path), "%s/etc/gdm/custom.conf", target_root);

    FILE *f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);

        if (size >= 0) {
            char *buf = malloc((size_t)size + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)size, f);
                buf[n] = '\0';
                fclose(f);

                const char *keys[] = {
                    "AutomaticLoginEnable=true\n",
                    "AutomaticLogin=liveuser\n"
                };

                for (size_t i = 0; i < 2; i++) {
                    char *p;
                    while ((p = strstr(buf, keys[i]))) {
                        size_t l = strlen(keys[i]);
                        memmove(p, p + l, strlen(p + l) + 1);
                    }
                }

                FILE *out = fopen(path, "w");
                if (out) {
                    fwrite(buf, 1, strlen(buf), out);
                    fclose(out);
                }
                free(buf);
            } else {
                fclose(f);
            }
        } else {
            fclose(f);
        }
    }

    /* AccountsService session configuration. */
    char accounts[1024];
    snprintf(accounts, sizeof(accounts),
             "%s/var/lib/AccountsService/users", target_root);
    mkdir(accounts, 0755);

    char account_file[1024];
    snprintf(account_file, sizeof(account_file),
             "%s/%s", accounts, username);

    snprintf(content, sizeof(content),
             "[User]\n"
             "Session=budgie-desktop-kwinwayland\n"
             "XSession=budgie-desktop-kwinwayland\n"
             "SystemAccount=false\n");
    write_file(account_file, content);

    /* Remove liveuser if present. */
    {
        char *argv[] = {
            (char *)"userdel", (char *)"-r",
            (char *)"liveuser", NULL
        };
        chroot_run(target_root, argv);
    }

    /* Build the list of groups that actually exist. */
    static const char *groups[] = {
        "wheel", "audio", "video", "input",
        "network", "storage", "power", "docker"
    };

    char group_list[256] = "";
    char skipped_list[256] = "";

    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        char *dst = group_exists(target_root, groups[i])
                 ? group_list : skipped_list;

        if (*dst)
            strncat(dst, ",", 255 - strlen(dst));

        strncat(dst, groups[i], 255 - strlen(dst));
    }

    /* Create user if it doesn't already exist. */
    char passwd[1024];
    snprintf(passwd, sizeof(passwd), "%s/etc/passwd", target_root);

    if (!file_contains_user(passwd, username)) {
        char *argv[] = {
            (char *)"useradd", (char *)"-m",
            (char *)"-G", group_list,
            (char *)"-s", (char *)"/bin/bash",
            (char *)username, NULL
        };

        if (chroot_run(target_root, argv)) {
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "useradd failed for %s", username);
            return -1;
        }
    }

    /* Save groups that aren't available yet for first boot. */
    if (*skipped_list) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/etc/kibaos", target_root);
        mkdir(dir, 0755);

        char marker[1024];
        snprintf(marker, sizeof(marker),
                 "%s/pending-user-groups", dir);

        snprintf(content, sizeof(content),
                 "%s:%s\n", username, skipped_list);
        write_file(marker, content);
    }

    /* Set password through stdin, never argv. */
    {
        char passwd_data[512];
        snprintf(passwd_data, sizeof(passwd_data),
                 "%s:%s\n", username, password);

        char *argv[] = {
            (char *)"arch-chroot",
            (char *)target_root,
            (char *)"chpasswd",
            NULL
        };

        if (run_argv_with_stdin(argv, passwd_data)) {
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "chpasswd failed");
            return -1;
        }
    }

    /* Temporary password stash for WinApps first-run setup. */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/etc/kibaos", target_root);
        mkdir(dir, 0755);

        char passfile[1024];
        snprintf(passfile, sizeof(passfile),
                 "%s/winapps-userpass", dir);

        int fd = open(passfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            size_t len = strlen(password), done = 0;

            while (done < len) {
                ssize_t n = write(fd, password + done, len - done);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                done += (size_t)n;
            }

            close(fd);
        }
    }

    /* Ensure ~/.local/bin exists for WinApps. */
    {
        char home_bin[1024];
        snprintf(home_bin, sizeof(home_bin),
                 "/home/%s/.local/bin", username);

        char *argv[] = {
            (char *)"install", (char *)"-d",
            (char *)"-m", (char *)"755",
            (char *)"-o", (char *)username,
            (char *)"-g", (char *)username,
            home_bin, NULL
        };

        chroot_run(target_root, argv);
    }

    return 0;
}

int kiba_install_finalize(const char *target_root,
                          const char *disk_path,
                          const char *root_part,
                          const char *root_uuid,
                          int root_partno,
                          bool dualboot,
                          kiba_progress_cb cb,
                          void *user_data)
{
    char path[1024];

    if (cb)
        cb(80, "Cleaning up installer files...", user_data);

    static const char *live_only[] = {
        "usr/share/applications/kibaos-install.desktop",
        "usr/bin/io.kibaos.oobe",
        "usr/share/kibaos-oobe",
        "usr/local/bin/kibaos-oobe-backend",
        "usr/local/bin/kibaos-oem-finish.sh",
        "etc/systemd/system/choose-mirror.service",
        "usr/share/libalpm/hooks/Installation_guide.hook",
        "root/customize_airootfs.sh",
        "root/install.txt",
        "etc/motd",
        "etc/issue"
    };

    for (size_t i = 0; i < sizeof(live_only) / sizeof(live_only[0]); i++) {
        snprintf(path, sizeof(path),
                 "%s/%s", target_root, live_only[i]);

        char *argv[] = {
            (char *)"rm", (char *)"-rf", path, NULL
        };
        run_argv(argv);
    }

    /* Remove live-only ArchISO tooling. */
    {
        char *argv[] = {
            (char *)"pacman", (char *)"-Rns",
            (char *)"--noconfirm",
            (char *)"archiso",
            (char *)"mkinitcpio-archiso",
            (char *)"squashfs-tools",
            NULL
        };
        chroot_run(target_root, argv);
    }

    if (cb)
        cb(84, "Setting up your computer to start KibaOS...", user_data);

    /* KibaOS requires UEFI. */
    if (access("/sys/firmware/efi", F_OK) != 0) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "KibaOS requires UEFI boot. "
                 "The installer appears to be running in BIOS/legacy mode. "
                 "If this is a VM, enable UEFI firmware.");
        return -1;
    }

    /* Install systemd-boot. */
    {
        char *argv[] = {
            (char *)"bootctl",
            (char *)"--esp-path=/boot",
            (char *)"install",
            NULL
        };

        if (chroot_run(target_root, argv)) {
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "bootctl install failed");
            return -1;
        }
    }

    if (cb)
        cb(86, "Building your boot menu...", user_data);

    /* systemd-boot configuration. */
    snprintf(path, sizeof(path),
             "%s/boot/loader/loader.conf", target_root);

    char loader[256];
    snprintf(loader, sizeof(loader),
             "default kibaos.conf\n"
             "timeout %d\n"
             "console-mode max\n"
             "editor no\n",
             dualboot ? 5 : 0);

    if (write_file(path, loader)) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "writing loader.conf failed");
        return -1;
    }

    snprintf(path, sizeof(path),
             "%s/boot/loader/entries", target_root);
    mkdir(path, 0755);

    snprintf(path, sizeof(path),
             "%s/boot/loader/entries/kibaos.conf", target_root);

    char entry[1024];
    snprintf(entry, sizeof(entry),
             "title KibaOS\n"
             "linux /vmlinuz-linux\n"
             "initrd /initramfs-linux.img\n"
             "options root=UUID=%s rw quiet splash loglevel=3 "
             "rd.udev.log_level=3 vt.global_cursor_default=0 "
             "plymouth.use-simpledrm=1 "
             "lsm=landlock,lockdown,yama,integrity,apparmor,bpf\n",
             root_uuid);

    if (write_file(path, entry)) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "writing boot entry failed");
        return -1;
    }

    (void)disk_path;
    (void)root_part;
    (void)root_partno;

    if (cb)
        cb(88, "Turning on background features...", user_data);

    /* Graphical boot target. */
    {
        char *argv[] = {
            (char *)"systemctl",
            (char *)"set-default",
            (char *)"graphical.target",
            NULL
        };

        if (chroot_run(target_root, argv)) {
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "setting graphical.target failed");
            return -1;
        }
    }

    static const char *services[] = {
        "NetworkManager",
        "gdm",
        "bluetooth",
        "systemd-timesyncd",
        "systemd-time-wait-sync",
        "kibaos-panelfix"
    };

    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        char *argv[] = {
            (char *)"systemctl",
            (char *)"enable",
            (char *)services[i],
            NULL
        };
        chroot_run(target_root, argv);
    }

    if (cb)
        cb(89, "Saving your Wi-Fi settings...", user_data);

    /* Copy NetworkManager profiles from live environment. */
    snprintf(path, sizeof(path),
             "%s/etc/NetworkManager/system-connections", target_root);
    mkdir(path, 0700);

    {
        char *argv[] = {
            (char *)"bash", (char *)"-c",
            (char *)
            "cp -a /etc/NetworkManager/system-connections/. \"$1\"/ "
            "2>/dev/null; "
            "chown -R root:root \"$1\"; "
            "find \"$1\" -type f -exec chmod 600 {} +",
            (char *)"--", path, NULL
        };
        run_argv(argv);
    }

    if (cb)
        cb(90, "Adding support for videos and music...", user_data);

    /* Multimedia codecs. */
    {
        char *argv[] = {
            (char *)"pacman", (char *)"-S",
            (char *)"--noconfirm", (char *)"--needed",
            (char *)"gst-plugins-ugly",
            (char *)"gst-libav",
            (char *)"ffmpeg",
            NULL
        };

        if (chroot_run(target_root, argv) && cb)
            cb(90,
               "Couldn't add video/music support right now "
               "(no internet?) — you can add it later from Settings",
               user_data);
    }

    if (cb)
        cb(91, "Adding your startup screen...", user_data);

    /* Quiet kernel messages. */
    snprintf(path, sizeof(path),
             "%s/etc/sysctl.d", target_root);
    mkdir(path, 0755);

    snprintf(path, sizeof(path),
             "%s/etc/sysctl.d/20-quiet-printk.conf", target_root);
    write_file(path, "kernel.printk = 3 3 3 3\n");

    /* Plymouth. */
    snprintf(path, sizeof(path),
             "%s/etc/plymouth", target_root);
    mkdir(path, 0755);

    snprintf(path, sizeof(path),
             "%s/etc/plymouth/plymouthd.conf", target_root);

    write_file(path,
               "[Daemon]\n"
               "Theme=kibaos\n"
               "ShowDelay=0\n"
               "DeviceTimeout=8\n");

    char theme[1024];
    snprintf(theme, sizeof(theme),
             "%s/usr/share/plymouth/themes/kibaos/kibaos.plymouth",
             target_root);

    struct stat st;
    if (stat(theme, &st) != 0) {
        if (cb)
            cb(93,
               "Couldn't find the KibaOS startup screen — "
               "using the default one instead",
               user_data);
    } else {
        char *argv[] = {
            (char *)"plymouth-set-default-theme",
            (char *)"kibaos",
            NULL
        };

        if (chroot_run(target_root, argv) && cb)
            cb(93,
               "Couldn't set the KibaOS startup screen — "
               "using the default one instead",
               user_data);
    }

    if (cb)
        cb(94, "Finishing up...", user_data);

    /* Find the installed kernel version. */
    char kver[256] = {0};
    char modules[1024];

    snprintf(modules, sizeof(modules),
             "%s/usr/lib/modules", target_root);

    DIR *d = opendir(modules);
    if (d) {
        struct dirent *ent;

        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.')
                continue;

            char full[1200];
            snprintf(full, sizeof(full),
                     "%s/%s", modules, ent->d_name);

            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(kver, sizeof(kver),
                         "%s", ent->d_name);
                break;
            }
        }

        closedir(d);
    }

    if (!*kver) {
        snprintf(g_finish_err, sizeof(g_finish_err),
                 "couldn't find an installed kernel under "
                 "/usr/lib/modules");
        return -1;
    }

    /* Rebuild initramfs against the installed kernel, not uname -r. */
    {
        char *argv[] = {
            (char *)"mkinitcpio",
            (char *)"-c",
            (char *)"/etc/mkinitcpio.conf.d/installed.conf",
            (char *)"-g",
            (char *)"/boot/initramfs-linux.img",
            (char *)"-k",
            kver,
            NULL
        };

        if (chroot_run(target_root, argv)) {
            snprintf(g_finish_err, sizeof(g_finish_err),
                     "mkinitcpio failed");
            return -1;
        }
    }

    /* Wheel sudo access. */
    snprintf(path, sizeof(path),
             "%s/etc/sudoers.d/wheel", target_root);

    if (write_file(path, "%wheel ALL=(ALL:ALL) ALL\n") == 0)
        chmod(path, 0440);

    return 0;
}

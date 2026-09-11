/* kiba_install.h — the rest of the install pipeline: extracting the
 * live squashfs onto the new root, writing fstab/hostname/locale,
 * creating the user account, installing the bootloader, and enabling
 * services.
 *
 * Same rule as kiba_fs.c: no shell, no string-parsing of subprocess
 * stdout. Where a maintained external tool is the only sane
 * implementation of something complex (unsquashfs's LZMA/xz/zstd
 * decompression, arch-chroot's mount namespace setup, bootctl's
 * systemd-boot installation), it's invoked via posix_spawnp with a
 * literal argv array -- never system()/popen(), so there's no shell
 * to inject into and no string protocol to desync.
 *
 * Functions that report progress take a kiba_progress_cb so the caller
 * (the GTK4 app, in-process) gets typed callbacks instead of scraping
 * "PROGRESS N msg" lines from stdout.
 */
#ifndef KIBA_INSTALL_H
#define KIBA_INSTALL_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*kiba_progress_cb)(int pct, const char *msg, void *user_data);

/* Locates the live squashfs/erofs image on the boot medium. Writes the
 * found path into out_path (caller-provided buffer). Returns true on
 * success. Mirrors the find-strategy from the old Python backend
 * (findmnt-based, with conventional-path and full-scan fallbacks) but
 * implemented by walking /proc/self/mountinfo directly instead of
 * shelling out to `findmnt`. */
bool kiba_find_live_image(char *out_path, size_t out_len);

/* Extracts the squashfs/erofs image found above onto `target_root`.
 * For squashfs: posix_spawnp's `unsquashfs -f -d <target> <image>`.
 * For erofs: mount(2) the image read-only via a loop device, then
 * recursively copy (our own copy, not `cp -a`) onto target_root. */
int kiba_install_extract_image(const char *image_path, const char *target_root,
                                kiba_progress_cb cb, void *user_data);

/* mkarchiso's _cleanup_pacstrap_dir() deletes everything under
 * pacstrap_dir/boot (including vmlinuz-linux and initramfs-linux.img)
 * *before* the airootfs image is built, so the kernel is never actually
 * inside the squashfs/erofs image kiba_install_extract_image just
 * extracted -- it only exists on the boot medium, copied there
 * separately by mkarchiso alongside the image (as a sibling "boot/"
 * dir next to the arch dir holding airootfs.sfs/.erofs). Must be
 * called once, right after kiba_install_extract_image succeeds, or
 * the installed system boots to nothing. image_path is the same path
 * kiba_find_live_image returned. */
int kiba_install_copy_kernel(const char *image_path, const char *target_root);

/* Writes /etc/fstab, /etc/hostname, /etc/hosts, locale.conf,
 * vconsole.conf directly (plain file I/O, not even posix_spawn). */
int kiba_install_write_configs(const char *target_root,
                                const char *root_uuid, const char *esp_uuid,
                                const char *hostname, const char *locale,
                                const char *keymap);

/* Runs locale-gen inside the chroot (posix_spawnp arch-chroot). */
int kiba_install_locale_gen(const char *target_root);

/* Removes the live user, creates the real user account, sets password.
 * useradd/userdel/chpasswd are run inside the chroot via posix_spawnp
 * (no shell); chpasswd's input is written to its stdin pipe directly,
 * never formatted into a shell string. */
int kiba_install_create_user(const char *target_root, const char *username,
                              const char *password);

/* Removes live-only files/packages, installs the bootloader via
 * posix_spawnp arch-chroot bootctl (systemd-boot) plus hand-written
 * loader.conf/entry files, enables services, rebuilds the initramfs.
 * This only affects the INSTALLED system's bootloader -- the live ISO
 * itself still boots via its own archiso-managed boot stub (GRUB on
 * x86_64, systemd-boot on aarch64 -- see the archiso profile config),
 * this function is never invoked for the ISO build. bootctl needs no
 * --target/arch flag the way grub-install did: it ships one binary per
 * arch and always installs the one matching itself, so the same call
 * works unmodified on both x86_64 and aarch64 targets. */
/* root_partno/disk_path are no longer used by the systemd-boot path
 * (the boot entry is written directly from root_uuid, which the
 * caller already resolved from the freshly-formatted filesystem) but
 * are kept in the signature for compatibility with the rest of the
 * install pipeline. */
/* dualboot: when true, the ESP being installed to is shared with an
 * existing OS. We leave that OS's own boot files on the ESP completely
 * untouched (bootctl install only ever adds systemd-boot's own files
 * under /EFI/systemd/ and /EFI/BOOT/, plus an NVRAM entry -- it never
 * removes anyone else's). Like rEFInd before it, systemd-boot
 * auto-discovers other EFI bootloaders already present on the ESP on
 * its own per the Boot Loader Specification (no os-prober equivalent
 * needed) -- we just give it a longer timeout on dual-boot installs so
 * that menu is actually visible instead of auto-booting straight into
 * KibaOS. We deliberately write the boot entry ourselves from
 * root_uuid rather than relying on any auto-generation, for the same
 * reason the old rEFInd path did: no chroot/live-environment
 * kernel-parameter footguns to worry about. */
int kiba_install_finalize(const char *target_root, const char *disk_path,
                           const char *root_part, const char *root_uuid,
                           int root_partno, bool dualboot,
                           kiba_progress_cb cb, void *user_data);

/* Human-readable description of the last failure from any kiba_install_*
 * function in this module. */
const char *kiba_install_strerror(void);

#endif

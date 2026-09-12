/* kiba_install.h — the rest of the install pipeline: extracting the
 * live squashfs onto the new root, writing fstab/hostname,
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

/* Locates the live squashfs/erofs image on the boot medium.
 * Writes the found path into out_path (caller-provided buffer).
 * Returns true on success.
 *
 * Mirrors the find-strategy from the old Python backend
 * (findmnt-based, with conventional-path and full-scan fallbacks)
 * but is implemented by walking /proc/self/mountinfo directly
 * instead of shelling out to findmnt.
 */
bool kiba_find_live_image(char *out_path, size_t out_len);

/* Extracts the squashfs/erofs image found above onto target_root.
 *
 * For squashfs: posix_spawnp's
 *   unsquashfs -f -d <target> <image>
 *
 * For erofs: mounts the image read-only via a loop device, then
 * recursively copies it onto target_root using our own copy code.
 */
int kiba_install_extract_image(const char *image_path,
                               const char *target_root,
                               kiba_progress_cb cb,
                               void *user_data);

/* mkarchiso's _cleanup_pacstrap_dir() deletes everything under
 * pacstrap_dir/boot (including vmlinuz-linux and initramfs-linux.img)
 * before the airootfs image is built.
 *
 * Therefore the kernel is not inside the squashfs/erofs image that
 * kiba_install_extract_image() extracts. It exists on the boot medium
 * separately, alongside the image.
 *
 * Must be called once immediately after
 * kiba_install_extract_image() succeeds, or the installed system
 * will have no kernel/initramfs to boot.
 *
 * image_path is the same path returned by kiba_find_live_image().
 */
int kiba_install_copy_kernel(const char *image_path,
                             const char *target_root);

/* Writes /etc/fstab, /etc/hostname, /etc/hosts and
 * /etc/vconsole.conf directly using normal file I/O.
 *
 * Locale generation and locale.conf handling are intentionally not
 * performed here.
 */
int kiba_install_write_configs(const char *target_root,
                               const char *root_uuid,
                               const char *esp_uuid,
                               const char *hostname,
                               const char *keymap);

/* Removes the live user, creates the real user account and sets its
 * password.
 *
 * useradd, userdel and chpasswd are run inside the chroot via
 * posix_spawnp with literal argv arrays. chpasswd's input is written
 * directly to its stdin pipe; no shell command or shell string is used.
 */
int kiba_install_create_user(const char *target_root,
                             const char *username,
                             const char *password);

/* Removes live-only files/packages, installs the bootloader via
 * arch-chroot + bootctl (systemd-boot), writes loader.conf and the
 * KibaOS boot entry, enables services and rebuilds the initramfs.
 *
 * This only affects the INSTALLED system's bootloader. The live ISO
 * continues to use its own archiso-managed boot stub.
 *
 * root_partno and disk_path are no longer used by the systemd-boot
 * path, but remain in the signature for compatibility with the rest
 * of the install pipeline.
 *
 * dualboot:
 *   When true, the ESP is shared with another operating system.
 *   Existing OS boot files are left untouched. systemd-boot is
 *   installed alongside them and receives a longer menu timeout so
 *   other EFI bootloaders can be selected.
 *
 * The KibaOS boot entry is written directly from root_uuid rather
 * than relying on automatic kernel-parameter generation.
 */
int kiba_install_finalize(const char *target_root,
                          const char *disk_path,
                          const char *root_part,
                          const char *root_uuid,
                          int root_partno,
                          bool dualboot,
                          kiba_progress_cb cb,
                          void *user_data);

/* Human-readable description of the last failure from any
 * kiba_install_* function in this module.
 */
const char *kiba_install_strerror(void);

#endif /* KIBA_INSTALL_H */

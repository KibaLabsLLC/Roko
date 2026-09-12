#!/bin/bash
# Builds the KibaOS OOBE installer (frontend + privileged backend) in
# isolation, outside the full ISO build. Mirrors the exact sequence run
# inline inside build.sh's customize_airootfs.sh chroot.
set -ex

pacman -S --noconfirm --needed gtk4 libadwaita libgee vala meson ninja base-devel \
    rsync polkit arch-install-scripts dosfstools gptfdisk squashfs-tools

# arch-install-scripts -> arch-chroot: everything kiba_install_finish.c's
# chroot_run() shells out to (bootctl, locale-gen, useradd, chpasswd,
# mkinitcpio) goes through arch-chroot. Without this package it's just
# absent from the system -- every one of those calls fails immediately
# (ENOENT) regardless of which disk or which machine this runs on. Do not
# drop this from the dependency list again.
# dosfstools -> mkfs.fat, used by kiba_fs.c to format the ESP.
# polkit -> pkexec, used by winapps-setup.vala's own elevation calls.
# gptfdisk -> sgdisk, used by kiba_gpt.c to write the GPT.
# squashfs-tools -> unsquashfs, used by kiba_install_extract_image().

# ── OOBE frontend (Vala/GTK4/libadwaita): io.kibaos.oobe + io.kibaos.winapps-setup
cd "$(dirname "$0")/src"
meson setup build --prefix=/usr || { echo "FATAL: meson setup failed for kibaos-oobe — check vala/gtk4/libadwaita dev package availability." >&2; exit 1; }
ninja -C build || { echo "FATAL: ninja build failed for kibaos-oobe — check the Vala compile errors above." >&2; exit 1; }
# Unconditional, not optional: the original build.sh installs immediately
# after building (one meson project, one `ninja -C build install` covers
# both io.kibaos.oobe and io.kibaos.winapps-setup) and then verifies
# /usr/bin/io.kibaos.oobe exists before continuing. Leaving this commented
# out -- as an earlier version of this script did -- means that check
# fails every time, since nothing ever put the binary at /usr/bin in the
# first place. Needs root (installs under --prefix=/usr).
ninja -C build install
cd -

# ── Privileged backend: libkibadisk + kibaos-oobe-backend
cd "$(dirname "$0")/src/disk"
make || { echo "FATAL: make failed for kibaos-oobe-backend — check the C compile errors above." >&2; exit 1; }
# `make` only compiles the binary into this directory — it does NOT
# install it anywhere on $PATH. Without this step the binary sits here,
# never makes it onto the ISO's squashfs, and the frontend's spawn of
# "kibaos-oobe-backend" fails with ENOENT at install time on a real
# machine, silently, with no log written (log_init() never even runs).
# Do not drop this line again.
install -Dm755 kibaos-oobe-backend /usr/local/bin/kibaos-oobe-backend
cd -

# Checks BOTH binaries now. The old version of this check only looked
# for io.kibaos.oobe (the frontend) and printed a success message even
# when kibaos-oobe-backend was never installed — which is exactly the
# bug that got us here. Don't narrow this back down to one binary.
if [ -x /usr/bin/io.kibaos.oobe ] && [ -x /usr/local/bin/kibaos-oobe-backend ]; then
  echo "=== KibaOS OOBE installer is the active install path ==="
else
  echo "=== WARNING: KibaOS OOBE installer binary not found post-build ===" >&2
  [ -x /usr/bin/io.kibaos.oobe ]              || echo "    missing: /usr/bin/io.kibaos.oobe" >&2
  [ -x /usr/local/bin/kibaos-oobe-backend ]   || echo "    missing: /usr/local/bin/kibaos-oobe-backend" >&2
  exit 1
fi

echo "Build complete: /usr/bin/io.kibaos.oobe, /usr/bin/io.kibaos.winapps-setup, /usr/local/bin/kibaos-oobe-backend"

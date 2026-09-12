#!/bin/bash
# Builds the KibaOS OOBE installer (frontend + privileged backend) in
# isolation, outside the full ISO build. Mirrors the exact sequence run
# inline inside build.sh's customize_airootfs.sh chroot.
set -ex

pacman -S --noconfirm --needed gtk4 libadwaita libgee vala meson ninja base-devel \
    rsync polkit arch-install-scripts dosfstools

# arch-install-scripts -> arch-chroot: everything kiba_install_finish.c's
# chroot_run() shells out to (bootctl, locale-gen, useradd, chpasswd,
# mkinitcpio) goes through arch-chroot. Without this package it's just
# absent from the system -- every one of those calls fails immediately
# (ENOENT) regardless of which disk or which machine this runs on. Do not
# drop this from the dependency list again.
# dosfstools -> mkfs.fat, used by kiba_fs.c to format the ESP.
# polkit -> pkexec, used by winapps-setup.vala's own elevation calls.

# ── OOBE frontend (Vala/GTK4/libadwaita): io.kibaos.oobe + io.kibaos.winapps-setup
cd "$(dirname "$0")/src"
meson setup build --prefix=/usr
ninja -C build
# ninja -C build install   # uncomment to install onto this machine
cd -

# ── Privileged backend: libkibadisk + kibaos-oobe-backend
cd "$(dirname "$0")/src/disk"
make
# make install   # uncomment to install onto this machine
cd -

echo "Build complete: src/build/io.kibaos.oobe, src/build/io.kibaos.winapps-setup, src/disk/kibaos-oobe-backend"

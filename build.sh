#!/bin/bash
# Builds the KibaOS OOBE installer (frontend + privileged backend) in
# isolation, outside the full ISO build. Mirrors the exact sequence run
# inline inside build.sh's customize_airootfs.sh chroot.
set -ex

pacman -S --noconfirm --needed gtk4 libadwaita libgee vala meson ninja base-devel

# ── OOBE frontend (Vala/GTK4/libadwaita): io.kibaos.oobe + io.kibaos.winapps-setup
cd "$(dirname "$0")/src"
meson setup build --prefix=/usr
ninja -C build
ninja -C build install   # uncomment to install onto this machine
cd -

# ── Privileged backend: libkibadisk + kibaos-oobe-backend
cd "$(dirname "$0")/src/disk"
make
make install   # uncomment to install onto this machine
cd -

echo "Build complete: src/build/io.kibaos.oobe, src/build/io.kibaos.winapps-setup, src/disk/kibaos-oobe-backend"

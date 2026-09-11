# ✦  Rōkō (狼吼) — “wolf's howl” ✦ 

### The fullscreen, one-step-per-screen out-of-box-experience installer for KibaOS.

## Layout

```
src/
  main.vala                 # OOBE app: NavigationView stack, one page per
                             # step (welcome, locale, disk, account,
                             # confirm, installing, done)
  winapps-setup.vala        # io.kibaos.winapps-setup — standalone,
                             # re-runnable Windows Workspace setup UI
  meson.build                # builds both Vala executables
  disk/                      # libkibadisk — privileged install backend
    kiba_gpt.h / kiba_gpt.c          # GPT partition table writer (sgdisk
                                      # via argv/execvp, never a shell)
    kiba_fs.h / kiba_fs.c            # mkfs/mount wrapper
    kiba_udev.h / kiba_udev.c        # hand-rolled udev-settle wait
    kiba_install.h                   # shared install-step declarations
    kiba_install_extract.c           # squashfs extraction + base install
    kiba_install_finish.c            # bootloader, locale, user account,
                                      # finalization steps
    kibaos_oobe_backend_main.c       # orchestrator; builds into
                                      # /usr/local/bin/kibaos-oobe-backend
    Makefile                         # builds libkibadisk.a + the backend
oobe.css                     # white-card design language, navy/glass
                              # KibaOS palette, light + dark
scripts/
  kibaos-oem-finish.sh        # OEM-mode completion (already-installed
                               # system: locale/keyboard/account only,
                               # no partitioning/bootloader work)
  kibaos-oem-prepare          # OEM-mode preparation (temporary "oem"
                               # autologin account + pending marker)
build.sh                      # builds everything in isolation
```

## Design

- **UI**: a `NavigationView` stack, one page per step. No sidebar, no
  visible step list — deliberately matches a Windows-OOBE
  single-question-per-screen feel, themed in KibaOS's own navy/glass
  palette (see `oobe.css`).
- **Backend**: a privileged C binary, `kibaos-oobe-backend`, invoked via
  `sudo` with a plain argv array — no shell string, no
  quoting/injection surface, no D-Bus/polkit dependency. GPT
  partitioning shells out to `sgdisk` via `execvp` (never a shell,
  never `parted`/`archinstall`/`libfdisk`). Whatever else is left
  (`unsquashfs`, `mkfs.fat`, `mkfs.ext4`, `arch-chroot`, `bootctl`,
  `mkinitcpio`, `useradd`/`chpasswd`, `locale-gen`, `pacman`) is invoked
  the same way, via argv arrays through `posix_spawn`, never a shell.

## Building

```sh
./build.sh
```

Needs `gtk4`, `libadwaita`, `libgee`, `vala`, `meson`, `ninja`, and a C
toolchain (`base-devel` on Arch). Produces:

- `src/build/io.kibaos.oobe` — the OOBE installer
- `src/build/io.kibaos.winapps-setup` — the Windows Workspace setup UI
- `src/disk/kibaos-oobe-backend` — the privileged backend, statically
  linked against `libkibadisk.a`

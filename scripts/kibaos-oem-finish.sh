#!/usr/bin/env bash
# Args: $1=locale $2=keymap $3=hostname $4=username $5=password
set -euo pipefail

LOCALE="$1"; KEYMAP="$2"; HOSTNAME_VAL="$3"; USERNAME_VAL="$4"; PASSWORD_VAL="$5"
LOG=/var/log/kibaos-oobe.log
exec > >(tee -a "${LOG}") 2>&1

progress() { echo "PROGRESS $1 $2"; }
fail() { progress 100 "Setup failed: $1"; echo "FATAL: $1" >&2; exit 1; }

[ -n "${USERNAME_VAL}" ] || fail "no username given"

progress 15 "Setting locale and keyboard..."
sed -i "s/#${LOCALE}/${LOCALE}/" /etc/locale.gen 2>/dev/null || true
echo "LANG=${LOCALE}" > /etc/locale.conf
echo "KEYMAP=${KEYMAP}" > /etc/vconsole.conf
locale-gen || fail "locale-gen failed"

progress 45 "Setting computer name..."
echo "${HOSTNAME_VAL}" > /etc/hostname
sed -i "s/127.0.1.1.*/127.0.1.1\t${HOSTNAME_VAL}.localdomain ${HOSTNAME_VAL}/" /etc/hosts 2>/dev/null || true

progress 65 "Creating your account..."
useradd -m -G wheel,audio,video,input,network,storage,power,docker -s /bin/bash "${USERNAME_VAL}" \
  || fail "useradd failed"
echo "${USERNAME_VAL}:${PASSWORD_VAL}" | chpasswd || fail "chpasswd failed"

# Stash the plaintext password briefly, root-only, so first-login WinApps
# setup can reuse it as the Windows guest's login instead of a random
# string nobody's ever shown (see kibaos-winapps-setup, which reads this
# once via pkexec and deletes it right after). Mirrors what
# kiba_install_create_user() does for the disk-install path -- this is
# the OEM-finish equivalent of that same account-creation moment.
mkdir -p /etc/kibaos
umask 077
printf '%s' "${PASSWORD_VAL}" > /etc/kibaos/winapps-userpass
chmod 600 /etc/kibaos/winapps-userpass
umask 022

progress 85 "Cleaning up OEM account..."
# Remove the temporary OEM account created by kibaos-oem-prepare, if present.
userdel -r oem 2>/dev/null || true
rm -f /var/lib/AccountsService/users/oem 2>/dev/null || true
# GDM has no LightDM-style conf.d layering to just drop a higher-priority
# file from -- kibaos-oem-prepare stashed whatever custom.conf looked like
# before OEM mode (if anything) at custom.conf.pre-oem, so restore that;
# absent a stashed copy, just delete custom.conf outright so GDM falls
# back to its own default of no autologin and a normal login screen.
if [ -f /etc/gdm/custom.conf.pre-oem ]; then
  mv /etc/gdm/custom.conf.pre-oem /etc/gdm/custom.conf
else
  rm -f /etc/gdm/custom.conf 2>/dev/null || true
fi

progress 95 "Finishing up..."
mkdir -p /etc/kibaos
touch /etc/kibaos/winapps-pending
rm -f /etc/kibaos/oem-pending

progress 100 "Done"
exit 0

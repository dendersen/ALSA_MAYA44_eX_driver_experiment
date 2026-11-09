#!/bin/bash
# Install the driver through DKMS so it is rebuilt for every new kernel.
set -e
cd "$(dirname "$0")"
V=1.0
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"
KERNEL="$(uname -r)"
apt-get install -y dkms "linux-headers-$KERNEL"
if [ ! -e "/lib/modules/$KERNEL/build/Makefile" ]; then
    echo "Kernel headers for $KERNEL are not available at /lib/modules/$KERNEL/build." >&2
    echo "Install linux-headers-$KERNEL, then run this script again." >&2
    exit 1
fi
dkms remove snd-maya44ex/$V --all 2>/dev/null || true
rm -rf /usr/src/snd-maya44ex-$V
mkdir /usr/src/snd-maya44ex-$V
cp dkms.conf Makefile snd-maya44ex.c /usr/src/snd-maya44ex-$V/
dkms install snd-maya44ex/$V -k "$KERNEL"
rmmod snd_maya44ex 2>/dev/null || true
modprobe snd_maya44ex
dkms status

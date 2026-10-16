#!/bin/bash
# Verify real hardware rates when switching rates back to back on one module load.
cd "$(dirname "$0")" || exit 1
systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber pipewire pipewire-pulse
sudo rmmod snd_maya44ex 2>/dev/null
sudo insmod ./snd-maya44ex.ko unverified_rates=1 || exit 1
for r in 32000 44100 48000 44100 32000 48000 96000 88200 44100; do
	t0=$(date +%s.%N)
	aplay -q -D hw:eX,0 -f S32_LE -c 2 -r $r -d 5 /dev/zero
	t1=$(date +%s.%N)
	python3 -c "t=$t1-$t0; print('req=$r time=%.2f actual_rate=%d' % (t, round($r*5/t)))"
done
sudo rmmod snd_maya44ex
sudo insmod ./snd-maya44ex.ko
systemctl --user start pipewire pipewire-pulse wireplumber

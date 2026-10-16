#!/bin/bash
# Debug helper: measure the real hardware rate for candidate clock words.
# Requests 44.1 kHz; actual rate = 44100 * 5 s / elapsed.
cd "$(dirname "$0")" || exit 1
systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber pipewire pipewire-pulse
for c in 0x0031 0x4031 0x0831 0x4831 0x0a31; do
	for s in 0x0000 0x2000 0x3000 0xa000; do
		sudo rmmod snd_maya44ex 2>/dev/null
		sudo insmod ./snd-maya44ex.ko dbg_clk=$((c)) dbg_sel=$((s)) || continue
		t0=$(date +%s.%N)
		aplay -q -D hw:eX,0 -f S32_LE -c 2 -r 44100 -d 5 /dev/zero
		t1=$(date +%s.%N)
		python3 -c "t=$t1-$t0; print('clk=$c sel=$s time=%.2f actual_rate=%d' % (t, round(44100*5/t)))"
	done
done
sudo rmmod snd_maya44ex
sudo insmod ./snd-maya44ex.ko unverified_rates=1
systemctl --user start pipewire pipewire-pulse wireplumber

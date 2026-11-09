#!/bin/bash
# Crackle test: play a sine straight to the card (PipeWire stopped) and let
# aplay report underruns. Phase 1 = one pair, phase 2 = both pairs, the second
# stream joining 3 s later (the "late joiner" case).
cd "$(dirname "$0")" || exit 1
RATE=${1:-48000}
DUR=${2:-20}

sine() {	# $1 = frequency; endless S32_LE stereo sine
	python3 - "$1" "$RATE" <<'E'
import sys, math, struct
f, r = float(sys.argv[1]), int(sys.argv[2])
out = sys.stdout.buffer
i = 0
while True:
    blk = bytearray()
    for _ in range(r // 10):
        v = int(0x10000000 * math.sin(2 * math.pi * f * i / r)) & ~0xff
        blk += struct.pack('<ii', v, v)
        i += 1
    out.write(blk)
E
}

systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber pipewire pipewire-pulse
sudo rmmod snd_maya44ex 2>/dev/null
sudo insmod ./snd-maya44ex.ko unverified_rates=1 || exit 1
udevadm settle
for i in $(seq 50); do
	aplay -L 2>/dev/null | grep -q '^hw:CARD=eX,DEV=1' && break
	sleep 0.2
done
sleep 1

echo "== phase 1: pair 1/2 alone, ${DUR}s @ $RATE (440 Hz)"
timeout "$DUR" bash -c "$(declare -f sine); RATE=$RATE; sine 440" |
	aplay -D hw:eX,0 -f S32_LE -c 2 -r "$RATE" 2>&1 | grep -i -E "underrun|overrun|error" | sort | uniq -c
echo "-- phase 1 finished"

echo "== phase 2: pair 1/2 (440 Hz) + pair 3/4 (660 Hz, joins after 3 s), ${DUR}s"
timeout "$DUR" bash -c "$(declare -f sine); RATE=$RATE; sine 440" |
	aplay -D hw:eX,0 -f S32_LE -c 2 -r "$RATE" 2>&1 | grep -i -E "underrun|overrun|error" | sort | uniq -c &
sleep 3
timeout "$((DUR - 3))" bash -c "$(declare -f sine); RATE=$RATE; sine 660" |
	aplay -D hw:eX,1 -f S32_LE -c 2 -r "$RATE" 2>&1 | grep -i -E "underrun|overrun|error" | sort | uniq -c
wait
echo "== done (no lines above = no underruns reported)"

sudo rmmod snd_maya44ex
sudo insmod ./snd-maya44ex.ko
systemctl --user start pipewire pipewire-pulse wireplumber

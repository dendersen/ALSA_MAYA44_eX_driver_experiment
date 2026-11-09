#!/bin/bash
# Split a multichannel PipeWire pro-audio node into stereo pairs.
# usage: pw-split-pro.sh <sink|source> <node.name> <channels> [label]
# Writes ~/.config/pipewire/pipewire.conf.d/split-<node>.conf
set -e
dir=$1 node=$2 ch=$3 label=${4:-$2}
[[ $dir == sink || $dir == source ]] && [[ -n $node && $ch -ge 2 ]] || {
	sed -n 2,4p "$0"; exit 1; }
out=$HOME/.config/pipewire/pipewire.conf.d/split-$node.conf
mkdir -p "$(dirname "$out")"
{
echo "context.modules = ["
for ((i = 0; i + 1 < ch; i += 2)); do
	a=AUX$i b=AUX$((i + 1)); n="$label $((i + 1))$((i + 2))"
	if [[ $dir == sink ]]; then
		virt="capture.props  = { media.class = Audio/Sink node.name = \"split_${node}_$i\" audio.position = [ FL FR ] }"
		real="playback.props = { node.target = \"$node\" audio.position = [ $a $b ] stream.dont-remix = true node.passive = true }"
	else
		virt="playback.props = { media.class = Audio/Source node.name = \"split_${node}_$i\" audio.position = [ FL FR ] }"
		real="capture.props  = { node.target = \"$node\" audio.position = [ $a $b ] stream.dont-remix = true node.passive = true }"
	fi
	cat <<EOF
  { name = libpipewire-module-loopback
    args = {
      node.description = "$n"
      $virt
      $real
    }
  }
EOF
done
echo "]"
} > "$out"
echo "wrote $out; run: systemctl --user restart pipewire pipewire-pulse wireplumber"

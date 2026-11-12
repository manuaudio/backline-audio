#!/usr/bin/env bash
# Plugin capacity at 64 frames / 48 kHz, in process (dspbench).
# One "unit" = a 4-plugin channel strip: LSP Parametric EQ x8 (8 bells on),
# LSP Compressor Stereo, CHOWTapeModel, Dragonfly Hall Reverb.
# Usage: capacity.sh DSPBENCH SECONDS > results.jsonl
# SPDX-License-Identifier: MIT
set -u
B=${1:?dspbench binary}; S=${2:-20}
L=$HOME/.clap/lsp-plugins.clap
U=(--plugin "$L:in.lsp-plug.para_equalizer_x8_stereo:Filter type=1;Gain =3"
   --plugin "$L:in.lsp-plug.compressor_stereo"
   --plugin "$HOME/.clap/CHOWTapeModel.clap:"
   --plugin "$HOME/.clap/DragonflyHallReverb.clap:")
run() { # label args...
  local label=$1; shift
  local r; r=$("$B" "${U[@]}" --seconds "$S" "$@" 2>/dev/null | tail -1)
  echo "{\"label\": \"$label\", \"load1\": \"$(cut -d' ' -f1 /proc/loadavg)\", \"r\": $r}"
}
for w in sleep spin; do
  x=(); [ $w = spin ] && x=(--spin)
  for n in 1 8 12 16 18 20; do run "1thread-$w" --n $n --threads 1 --cpus 2 "${x[@]}"; done
  for n in 8 12 16 18; do run "6P-$w" --n $n --threads 6 --cpus 0,2,4,6,8,10 "${x[@]}"; done
  for n in 8 12 16; do run "12Pht-$w" --n $n --threads 12 --cpus 0,1,2,3,4,5,6,7,8,9,10,11 "${x[@]}"; done
  for n in 4 8 12; do run "16all-$w" --n $n --threads 16 --cpus 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 "${x[@]}"; done
done

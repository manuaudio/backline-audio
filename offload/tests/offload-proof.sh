#!/usr/bin/env bash
# The offload proof on one box, no sudo: a user network namespace
# (unshare -rn) with its own loopback and a veth pair. A server process runs a
# CLAP chain; the client (the console) sends one 64-frame stereo block per
# period and waits for it to come back inside the same period.
#   offload-proof.sh OFFLOAD_PROOF_BIN SECONDS > results.jsonl
# SPDX-License-Identifier: MIT
set -u
BIN=${1:?offload-proof binary}; S=${2:-30}
if [ -z "${BL_IN_NETNS:-}" ]; then
  exec unshare -rn env BL_IN_NETNS=1 bash "$0" "$@"
fi
ip link set lo up
ip link add veth0 type veth peer name veth1
ip link set veth0 up; ip link set veth1 up
L=$HOME/.clap/lsp-plugins.clap
LIGHT=(--plugin "$L:in.lsp-plug.para_equalizer_x8_stereo:Filter type=1;Gain =3" --plugin "$L:in.lsp-plug.compressor_stereo")
VERIFY=(--verify "$L:in.lsp-plug.para_equalizer_x8_stereo:Filter type=1;Gain =3" --verify "$L:in.lsp-plug.compressor_stereo")
HEAVY=()
for _ in 1 2 3 4 5 6; do
  HEAVY+=(--plugin "$L:in.lsp-plug.para_equalizer_x8_stereo:Filter type=1;Gain =3" --plugin "$L:in.lsp-plug.compressor_stereo"
          --plugin "$HOME/.clap/CHOWTapeModel.clap:" --plugin "$HOME/.clap/DragonflyHallReverb.clap:")
done
one() { # label transport wait chain(light|heavy) budget
  local label=$1 tr=$2 wait=$3 chain=$4 budget=$5 sargs=() cargs=() x=()
  [ "$wait" = spin ] && x=(--spin)
  if [ "$tr" = l2 ]; then sargs=(--if veth1); cargs=(--if veth0); else cargs=(--peer 127.0.0.1); fi
  local plug=("${LIGHT[@]}") ver=("${VERIFY[@]}")
  [ "$chain" = heavy ] && { plug=("${HEAVY[@]}"); ver=(); }
  "$BIN" server --transport "$tr" "${sargs[@]}" --cpu 4 "${x[@]}" "${plug[@]}" 2>/dev/null &
  local sp=$!
  sleep 2
  local r; r=$("$BIN" client --transport "$tr" "${cargs[@]}" --cpu 6 --seconds "$S" --budget-us "$budget" "${x[@]}" "${ver[@]}" 2>/dev/null)
  kill $sp 2>/dev/null; wait $sp 2>/dev/null
  echo "{\"label\": \"$label\", \"chain\": \"$chain\", \"load1\": \"$(cut -d' ' -f1 /proc/loadavg)\", \"r\": ${r:-null}}"
}
one udp-lo-block   udp block light 250
one udp-lo-spin    udp spin  light 250
one l2-veth-block  l2  block light 250
one l2-veth-spin   l2  spin  light 250
one l2-veth-spin-heavy  l2 spin  heavy 1333
one l2-veth-block-heavy l2 block heavy 1333

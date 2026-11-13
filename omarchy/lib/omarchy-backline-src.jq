# Where PipeWire is converting sample rates, from `pw-dump`: every audio
# stream whose rate differs from the device it plays to or records from.
#
# A stream's Format param holds the rate the app speaks; a device node's holds
# the rate its hardware runs at. From each stream, follow the links (down for
# playback, up for capture) to the hardware devices at the end, passing through
# filters and combined devices by their node.link-group, and compare the two
# rates. A stream that isn't linked, or whose format isn't settled yet, is not
# being converted. Used by omarchy-backline-src.
#
# Out: [{id, name, node, direction, from, to, device, deviceName, internal, combined}]
#   direction  "playback" (from = the stream's rate, to = the device's) or
#              "capture" (from = the device's rate, to = the stream's)
#   internal   the stream belongs to a filter or combined device, not an app
#   combined   for a Backline combined device's own member stream, the
#              device's name, so the Audio Setup panel can show it on its row

def audio_rate: ((.info.params.Format // [])[0].rate // 0);
def props: (.info.props // {});

[.[] | select(.type == "PipeWire:Interface:Node")
  | {id, props: props, rate: audio_rate}] as $nodes
| ($nodes | map({key: (.id | tostring), value: .}) | from_entries) as $by
| [.[] | select(.type == "PipeWire:Interface:Link") | .info
  | select((.state // "") != "error")
  | {out: .["output-node-id"], in: .["input-node-id"]}] as $links

| def is_device: (.props["device.id"] != null)
    and ((.props["media.class"] // "") | test("^Audio/(Sink|Source|Duplex)"));
  def group: (.props["node.link-group"] // null);
  def neighbours($down):
    .id as $id
    | [$links[] | select((if $down then .out else .in end) == $id)
      | $by[(if $down then .in else .out end) | tostring] | select(. != null)];
  def mates:
    group as $g | .id as $id
    | if $g == null then [] else [$nodes[] | select(group == $g and .id != $id)] end;

  # Breadth first from a stream to the devices at the end of its path. The
  # stream's own link-group is not followed: a combined device's member reaches
  # only its own device, not its siblings'.
  def devices($down):
    {queue: neighbours($down), seen: [.id], found: []}
    | until(.queue | length == 0;
        .queue[0] as $n
        | .queue |= .[1:]
        | if (.seen | index([$n.id])) then .
          elif ($n | is_device) then .seen += [$n.id] | .found += [$n]
          else .seen += [$n.id] | .queue += (($n | neighbours($down)) + ($n | mates))
          end)
    | .found;

  def combined_name:
    group as $g
    | if $g == null then null
      else first($nodes[] | select(group == $g)
        | select((.props["node.name"] // "") | startswith("backline_aggregate_"))
        | (.props["node.description"] // .props["node.name"])) // null
      end;

  [$nodes[]
    | select(.rate > 0)
    | (.props["media.class"] // "") as $class
    | select($class == "Stream/Output/Audio" or $class == "Stream/Input/Audio")
    | ($class == "Stream/Output/Audio") as $down
    | . as $s
    | devices($down)[]
    | select(.rate > 0 and .rate != $s.rate)
    | {
        id: $s.id,
        name: ($s.props["application.name"] // $s.props["node.description"] // $s.props["media.name"] // $s.props["node.name"] // ""),
        node: ($s.props["node.name"] // ""),
        direction: (if $down then "playback" else "capture" end),
        from: (if $down then $s.rate else .rate end),
        to: (if $down then .rate else $s.rate end),
        device: (.props["node.name"] // ""),
        deviceName: (.props["node.description"] // .props["node.nick"] // .props["node.name"] // ""),
        internal: (($s | group) != null),
        combined: ($s | combined_name)
      }]

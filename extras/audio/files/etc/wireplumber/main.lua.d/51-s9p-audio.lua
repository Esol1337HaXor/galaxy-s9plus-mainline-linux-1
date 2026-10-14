-- Galaxy S9+ internal audio: ABOX reports periods by polling (20 ms) and the
-- position moves in steps, which breaks timer-based scheduling (xruns).
table.insert(alsa_monitor.rules, {
  matches = { { { "node.name", "matches", "alsa_output.platform-s9p-abox-pcm.*" } } },
  apply_properties = {
    ["api.alsa.disable-tsched"] = true,
    ["api.alsa.period-size"] = 2048,
    ["api.alsa.period-num"] = 8,
    ["api.alsa.headroom"] = 2048,
  },
})

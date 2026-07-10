# 🗒️ Change log

## v2.0.0 (2025/07/01)

- Init src

## v2.1.0 (2026/07/12)

### Features
- **CameraFrame**: extend image format support (more raw/Bayer/YUV/compressed formats), add encoding helpers and Python bindings, and an "Auto" decode mode; preserve wire-compatible format values.
- **zenoh**: add a debug env toggle; disable gossip scouting by default.
- **trigger**: add `vlink-trigger`, an in-memory trigger-based event-data recorder (EDR) — rolling per-URL ring buffers over all discovered topics, dumping a pre/post window to a bag on trigger with file rotation; adds the `vlink::TriggerRecorder` extension engine and a Python binding. Dumps write in capture-time order by default, or reorder by true data-plane time when a `BagPluginInterface` reorder plugin is loaded (via `bag_plugin`), kept distinct from the post-dump `TriggerPluginInterface`.
- **bag-plugin**: make `BagPluginInterface::on_read` / `on_write` pure virtual and drop `VersionInfo` / `get_version_info` from `BagPluginInterface` (retained only on `SchemaPluginInterface`; the new `TriggerPluginInterface` never carried it). **Breaking**: the vtable layout changed, so the `BagPluginInterface` plugin major version is bumped to 2 — rebuild plugins against the new header and declare `VLINK_PLUGIN_DECLARE(..., 2, 0)`; hosts (`vlink-bag` / `vlink-dump` / `TriggerRecorder`) now request major 2 and reject old binaries.

### Improvements
- **viewer**: decode via FFmpeg threads (QImage only as fallback), fixing severe multi-camera JPEG lag; FFmpeg enabled by default; honor the image-type combo for zero-copy streams; fix help links and check for updates via the GitHub latest release.
- **webviz**: route supported CameraFrame formats through Foxglove and Rerun; fix Rerun 16/32/64-bit multi-channel image routing.
- **shm2**: loan large `Bytes` publishes to avoid copies; support no-fd iceoryx2 listeners with a non-busy wait path; raise default slice/memory size to 4 KiB.
- **eproto/efbs**: portable proto3 default-scalar printing (protobuf 3.21.12+), field-number-ordered output, sorted map entries, and valid JSON dumps.
- **bench**: auto-size shm2 runtime URLs from payload size and improve report grouping and consistency.

### Fixes
- Fix `proxy_server` `max_packet_size` handling.
- Fix bag-record crash on exit.
- Tighten image payload validation and unsafe-size handling.
- Stabilize DDS/DDSC and shm2 lifecycle tests against teardown races.

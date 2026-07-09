# 🗒️ Change log

## v2.0.0 (2025/07/01)

- Init src

## v2.1.0 (2026/07/09)

### Features
- **CameraFrame**: extend image format support (more raw/Bayer/YUV/compressed formats), add encoding helpers and Python bindings, and an "Auto" decode mode; preserve wire-compatible format values.
- **zenoh**: add a debug env toggle; disable gossip scouting by default.

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

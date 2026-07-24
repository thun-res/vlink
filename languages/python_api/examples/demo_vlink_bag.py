# Copyright (C) 2026 by Thun Lu. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License").
"""
demo_vlink_bag.py
=================

Tutorial-style demonstrations for VLink's bag (recording / replay) Python
API.

What a "bag" is
---------------
A VLink bag is a single ``.vdb`` (SQLite3) file that captures every
message published on a set of topics together with a small schema header
and timestamp index.  It is the equivalent of ``ros2 bag`` for the VLink
stack.  Bags are used for:

* Replaying production traffic during development / regression testing
* Capturing data for offline analysis (e.g. dataset curation)
* Time-travel debugging -- jump back to the moment a bug appeared
* Long-term archival of system behaviour

Two classes drive everything
----------------------------
``vlink.BagWriter``  -- records messages from a live process or from
                        user-supplied payloads into a ``.vdb`` file.
``vlink.BagReader``  -- opens a recorded ``.vdb`` and either replays
                        messages on a callback or just inspects metadata.

Both classes follow the same lifecycle:

    1.  ``cls.create(path, ...)`` -- factory that opens the file.
                                     Returns ``None`` on bad path / format.
    2.  Configure callbacks (``register_*_callback``) BEFORE running.
    3.  ``async_run()`` -- kick off the background thread (not needed when a
                           writer is created with ``sync_mode=True``).
    4.  ``push(frame)`` / ``play(...)`` -- drive the work.
    5.  For a writer, ``wait_for_idle(timeout_ms)`` -- drain accepted writes.
    6.  ``quit()`` then ``wait_for_quit(timeout_ms)`` -- stop the background loop.
    7.  ``BagWriter.close()`` -- finalize metadata/footer before checking ``fail()``.

Run
---
::

    python3 demo_vlink_bag.py

The script writes bags into a fresh ``tempfile.mkdtemp()`` directory, so
each invocation is independent and self-cleaning (the OS cleans up temp
files eventually).  No external services or .proto files are required;
each demo creates the bag it needs from scratch.

Mental model
------------
Picture a bag as a SQLite database with three logical tables:

    messages(timestamp_us, url, action_type, schema_type, ser_type, data)
    schemas (ser_type, encoding, schema_type, data)
    tags    (tag_name, timestamp_us)

``BagWriter.push(frame)`` inserts a row into ``messages``.  Schemas are
inserted automatically by ``register_schema_callback(...)`` or manually
via ``push_schema(...)``.  Tags are user-defined time markers inserted via
``BagReader.tag("name")`` during playback (mostly an editing aid).

``BagReader.play(config)`` walks the messages in timestamp order,
dispatching each one to the registered output callback, modulated by
``rate`` (1.0 = real-time, 2.0 = double speed; values <= 0 are normalised to 1.0).
"""

import os
import tempfile
import threading
import time

import vlink as _vlink  # type: ignore


# ===========================================================================
# Shared helpers
# ===========================================================================

def _make_temp_bag_path(name="demo.vdb"):
    """Return ``<fresh-tempdir>/<name>``.  Each demo uses its own path so
    they cannot interfere with each other."""
    return os.path.join(tempfile.mkdtemp(prefix="vlink_bag_demo_"), name)


def _bag_frame(url, ser_type, schema_type, action_type, data, timestamp=-1):
    frame = _vlink.Frame()
    frame.timestamp = timestamp
    frame.url = url
    frame.ser_type = ser_type
    frame.schema_type = schema_type
    frame.action_type = action_type
    frame.data = data
    return frame


# ===========================================================================
# Demo 1.  Smallest possible record / replay loop
# ===========================================================================

def demo_bag_simple_record_replay():
    """Tutorial: record 5 raw messages, then replay them via callback.

    This is the minimum useful bag pipeline.  Use it as a template for any
    workflow where you want to capture a bounded burst of payloads and
    play them back on-demand.
    """
    bag_path = _make_temp_bag_path("simple.vdb")

    # ---- Record --------------------------------------------------------
    # Default config: no compression, no splitting and sync_mode=False.
    writer = _vlink.BagWriter.create(bag_path)
    assert writer is not None, f"failed to create writer at {bag_path}"

    # Start the writer's background thread.  push(...) returns a non-negative
    # timestamp after enqueueing, or a negative value if admission is rejected;
    # the thread does the actual disk write.
    writer.async_run()

    # Push five messages on a single topic.  Each Frame carries the topic,
    # schema hints, action kind, payload, and optional explicit timestamp.
    for i in range(5):
        timestamp_us = writer.push(_bag_frame(
            "intra://demo/raw",            # topic URL
            "demo.Raw",                    # schema name (descriptive only)
            _vlink.SchemaType.Raw,         # the broker treats it as raw bytes
            _vlink.ActionType.Publish,     # this was a Publisher publish
            f"message-{i}".encode(),       # payload
        ))
        # push returns the timestamp the writer stamped on the row; useful
        # for time-correlated diagnostics.
        assert isinstance(timestamp_us, int)

    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    # ---- Replay --------------------------------------------------------
    # read_only=True: open without holding write locks (safe for concurrent
    # inspect / playback).
    reader = _vlink.BagReader.create(bag_path, read_only=True)
    assert reader is not None

    # Inspect metadata BEFORE playback to confirm what we wrote landed.
    info = reader.get_info()
    print(f"  recorded {info.message_count} messages, "
          f"{info.total_duration} ms total duration, "
          f"{info.file_size} bytes on disk")
    assert info.message_count == 5

    # Register the output callback.  It fires on the reader's background
    # thread for every message dispatched during play().
    received = []
    done = threading.Event()

    def on_output(frame):
        # frame.data is Python bytes; copy or decode here if you need it.
        received.append((frame.url, frame.action_type, bytes(frame.data)))

        if len(received) >= 5:
            done.set()

    reader.register_output_callback(on_output)

    # Configure the playback.  Defaults (rate=1.0, no auto_quit) replay
    # everything once at real-time speed.  We use 100x playback and auto_quit
    # so the reader stops on its own when done.
    cfg = _vlink.BagReader.Config()
    cfg.rate = 100.0
    cfg.auto_quit = True

    reader.async_run()
    reader.play(cfg)

    # Wait for all 5 callbacks to fire (or 3 s, whichever comes first).
    assert done.wait(timeout=3.0), f"only received {len(received)}/5"

    reader.quit()
    reader.wait_for_quit(5000)

    # Sanity check the payloads.
    payloads = [data for _url, _act, data in received]
    assert payloads == [b"message-0", b"message-1", b"message-2",
                         b"message-3", b"message-4"], f"got {payloads}"

    print("[OK] simple record + replay")


# ===========================================================================
# Demo 2.  Compression
# ===========================================================================

def demo_bag_with_compression():
    """Tutorial: enable LZAV compression for the VDB backend.

    Compression is set via ``BagWriter.Config.compress``.  AUTO picks the
    backend default; NONE disables compression entirely.  The built-in VDB
    writer uses LZAV, while the built-in MCAP writer uses Zstd or none.
    Use compression on long-running recordings or when bag files cross slow
    storage (NFS / cloud upload).
    """
    bag_path = _make_temp_bag_path("compressed.vdb")

    # Build a config explicitly so we can tweak it.
    cfg = _vlink.BagWriter.Config()
    cfg.compress = _vlink.BagWriter.CompressType.LZAV
    cfg.compress_start_size = 256                            # only compress rows >= 256 B
    cfg.tag_name = "demo_compressed_bag"                    # descriptive tag in metadata

    writer = _vlink.BagWriter.create(bag_path, cfg)
    assert writer is not None

    writer.async_run()

    # Push a bunch of large messages so the compressor can do something.
    payload = (b"VLink-bag-payload-" * 64)[:1024]
    for i in range(20):
        writer.push(_bag_frame(
            f"intra://demo/large/{i % 4}",  # spread across 4 topics
            "demo.Large",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            payload,
        ))

    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    # Read back to confirm the file is intact and compression metadata is
    # preserved.
    reader = _vlink.BagReader.create(bag_path, read_only=True)
    assert reader is not None

    info = reader.get_info()
    print(f"  compression={info.compression_type}, "
          f"raw_size={info.total_raw_size} B, file_size={info.file_size} B "
          f"(ratio={info.total_raw_size / max(info.file_size, 1):.2f}x)")

    assert info.compression_type, "bag is missing compression metadata"
    assert info.message_count == 20

    reader.quit()
    reader.wait_for_quit(5000)

    print("[OK] compressed bag")


# ===========================================================================
# Demo 3.  Recording / replaying zero-copy typed containers
# ===========================================================================

def demo_bag_zerocopy_record_replay():
    """Tutorial: record VLink zero-copy types into a bag, replay them
    typed.

    The bag only cares about (url, schema_name, schema_type, bytes).  When
    the payload is a zero-copy container, we:

      * Serialise with ``container.to_bytes()`` on the writer side.
      * Set ``schema_type`` to ``SchemaType.ZeroCopy`` so it matches the family
        the writer resolves from the ``vlink::zerocopy::*`` type name (a
        conflicting schema_type makes the writer reject the frame).
      * On the reader side, route per-url to the right ``from_bytes(...)``
        decoder.

    This pattern works for any zero-copy type: ``OccupancyGrid``,
    ``Tensor``, ``ObjectArray``, ``AudioFrame``, etc.
    """
    bag_path = _make_temp_bag_path("zerocopy.vdb")

    writer = _vlink.BagWriter.create(bag_path)
    assert writer is not None
    writer.async_run()

    # Build three CameraFrame messages.
    for seq, (w, h) in enumerate([(320, 240), (640, 480), (1280, 720)],
                                 start=1):
        frame = _vlink.CameraFrame()
        frame.header.seq = seq
        frame.set_width(w)
        frame.set_height(h)
        frame.set_format(_vlink.CameraFrame.Format.Nv12)
        assert frame.create(w * h * 3 // 2)

        writer.push(_bag_frame(
            "intra://demo/camera",                    # canonical topic
            "vlink::zerocopy::CameraFrame",
            _vlink.SchemaType.ZeroCopy,
            _vlink.ActionType.Publish,
            frame.to_bytes(),
        ))

    # Build two OccupancyGrid messages on a different topic.
    for seq, (w, h) in enumerate([(40, 40), (80, 60)], start=1):
        og = _vlink.OccupancyGrid()
        og.header.seq = seq
        og.set_width(w)
        og.set_height(h)
        og.set_resolution(0.05)
        og.set_cell_type(_vlink.OccupancyGrid.CellType.Int8)
        og.set_default_value(-1)
        og.set_map_id(f"map_{seq}")
        assert og.create(w * h)

        writer.push(_bag_frame(
            "intra://demo/map",
            "vlink::zerocopy::OccupancyGrid",
            _vlink.SchemaType.ZeroCopy,
            _vlink.ActionType.Publish,
            og.to_bytes(),
        ))

    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    # ---- Replay --------------------------------------------------------
    reader = _vlink.BagReader.create(bag_path, read_only=True)
    info = reader.get_info()
    print(f"  recorded {info.message_count} typed messages across "
          f"{len(info.url_metas)} topics")

    cameras = []
    grids = []
    done = threading.Event()
    expected_total = info.message_count

    def on_output(frame):
        # Dispatch by topic.  In a real consumer you would consult the
        # schema name (reader.get_ser_type(frame.url)) so per-topic decoders are
        # discovered dynamically.
        payload = _vlink.Bytes.from_bytes(frame.data)

        if frame.url == "intra://demo/camera":
            camera = _vlink.CameraFrame()
            assert camera.from_bytes(payload)
            cameras.append((camera.header.seq, camera.width(), camera.height()))
        elif frame.url == "intra://demo/map":
            og = _vlink.OccupancyGrid()
            assert og.from_bytes(payload)
            grids.append((og.header.seq, og.width(), og.height(), og.map_id()))

        if len(cameras) + len(grids) >= expected_total:
            done.set()

    reader.register_output_callback(on_output)

    cfg = _vlink.BagReader.Config()
    cfg.rate = 100.0         # accelerated playback
    cfg.auto_quit = True

    reader.async_run()
    reader.play(cfg)

    assert done.wait(timeout=3.0)
    reader.quit()
    reader.wait_for_quit(5000)

    assert cameras == [(1, 320, 240), (2, 640, 480), (3, 1280, 720)], f"got {cameras}"
    assert grids == [(1, 40, 40, "map_1"), (2, 80, 60, "map_2")], f"got {grids}"

    print("[OK] zero-copy record + replay")


# ===========================================================================
# Demo 4.  Playback control: rate, pause, resume, jump
# ===========================================================================

def demo_bag_playback_control():
    """Tutorial: pause / resume / jump during playback.

    Build a 1-second bag of timestamped messages, then exercise the
    playback control API:

        play(cfg)         -- start dispatching
        pause()           -- stop dispatching (keeps state)
        resume()          -- continue from pause
        jump(t_us, rate)  -- seek to absolute timestamp
    """
    bag_path = _make_temp_bag_path("control.vdb")

    writer = _vlink.BagWriter.create(bag_path)
    assert writer is not None
    writer.async_run()

    # Push 20 messages with explicit, monotonic timestamps spanning ~1 s.
    base_us = 1_000_000_000
    for i in range(20):
        writer.push(_bag_frame(
            "intra://demo/control",
            "demo.Pulse",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            f"pulse-{i:02d}".encode(),
            timestamp=base_us + i * 50_000,  # 50 ms apart -> 1 s span
        ))

    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    reader = _vlink.BagReader.create(bag_path, read_only=True)

    seen = []
    finished = threading.Event()

    def on_output(frame):
        seen.append((frame.timestamp, bytes(frame.data)))

    def on_finish(interrupted):
        finished.set()

    reader.register_output_callback(on_output)
    reader.register_finish_callback(on_finish)

    # ---- Burst playback at maximum speed -------------------------------
    cfg = _vlink.BagReader.Config()
    cfg.force_delay = 0
    cfg.auto_quit = False

    reader.async_run()
    reader.play(cfg)

    assert finished.wait(timeout=3.0)
    print(f"  full burst replayed {len(seen)} messages")
    assert len(seen) == 20

    # ---- jump() to a specific timestamp (mid-bag) ----------------------
    # Reset and replay the second half via jump().
    seen.clear()
    finished.clear()

    mid_ts_ms = (base_us + 10 * 50_000) // 1000
    reader.jump(mid_ts_ms, rate=1.0, force_to_play=True)

    assert finished.wait(timeout=3.0)
    payloads = [bytes(d) for _ts, d in seen]
    assert payloads == [f"pulse-{i:02d}".encode() for i in range(10, 20)], \
        f"got {payloads}"
    print(f"  jump -> {len(seen)} messages from t={mid_ts_ms} ms onward")

    reader.quit()
    reader.wait_for_quit(5000)
    print("[OK] playback control (rate / jump)")


# ===========================================================================
# Demo 5.  Filter URLs (replay a subset of topics)
# ===========================================================================

def demo_bag_filter_urls():
    """Tutorial: use ``Config.filter_urls`` to replay a subset of topics.

    A real recording usually contains many topics.  At replay time you
    often only care about a handful (e.g. when reproducing a perception
    bug, you want camera + detections, not also IMU + GPS + audio).
    """
    bag_path = _make_temp_bag_path("filter.vdb")

    writer = _vlink.BagWriter.create(bag_path)
    assert writer is not None
    writer.async_run()

    # Record three topics, three messages each.
    for topic in ("camera", "lidar", "imu"):
        for i in range(3):
            writer.push(_bag_frame(
                f"intra://demo/{topic}",
                f"demo.{topic.title()}",
                _vlink.SchemaType.Raw,
                _vlink.ActionType.Publish,
                f"{topic}-{i}".encode(),
            ))

    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    # ---- Replay only `camera` and `imu` --------------------------------
    reader = _vlink.BagReader.create(bag_path, read_only=True)

    received = []
    finished = threading.Event()
    reader.register_output_callback(lambda frame: received.append((frame.url, bytes(frame.data))))
    reader.register_finish_callback(lambda interrupted: finished.set())

    cfg = _vlink.BagReader.Config()
    cfg.filter_urls = {"intra://demo/camera", "intra://demo/imu"}
    cfg.rate = 100.0
    cfg.auto_quit = True

    reader.async_run()
    reader.play(cfg)
    assert finished.wait(timeout=3.0)

    reader.quit()
    reader.wait_for_quit(5000)

    # We should have NO lidar payloads.
    seen_topics = {url for url, _ in received}
    assert "intra://demo/lidar" not in seen_topics
    assert seen_topics == {"intra://demo/camera", "intra://demo/imu"}, \
        f"got {seen_topics}"
    print(f"  filtered playback: {len(received)} messages from "
          f"{sorted(seen_topics)}")
    print("[OK] filter URLs")


# ===========================================================================
# Demo 6.  Inspect-only mode (no playback)
# ===========================================================================

def demo_bag_inspect_only():
    """Tutorial: read bag metadata without playing it back.

    Used by tools that need to display a bag's contents -- ``vlink-bag
    info``, dataset curators, file browsers, etc.  ``get_info()`` returns
    a ``BagInfo`` with per-URL metadata; no payload bytes are decoded.
    """
    bag_path = _make_temp_bag_path("inspect.vdb")

    # Record a few topics so there's something to inspect.
    writer = _vlink.BagWriter.create(bag_path)
    writer.async_run()
    for i in range(7):
        writer.push(_bag_frame(
            "intra://demo/topic_a",
            "demo.A",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            f"a-{i}".encode(),
        ))
    for i in range(3):
        writer.push(_bag_frame(
            "intra://demo/topic_b",
            "demo.B",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            f"b-{i}".encode(),
        ))
    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail()
    del writer

    # ---- Inspect without playback --------------------------------------
    reader = _vlink.BagReader.create(bag_path, read_only=True)
    info = reader.get_info()

    print(f"  bag file        : {info.file_name}")
    print(f"  format version  : {info.version}")
    print(f"  storage         : {info.storage_type}")
    print(f"  compression     : {info.compression_type or 'NONE'}")
    print(f"  total messages  : {info.message_count}")
    print(f"  total duration  : {info.total_duration} ms")
    print(f"  total raw bytes : {info.total_raw_size}")
    print(f"  file bytes      : {info.file_size}")

    # Per-URL breakdown.  url_metas is iterable; each entry has count,
    # size, freq, action_type, schema info, etc.
    print("  per-topic:")
    for meta in info.url_metas:
        print(f"    - {meta.url:30s}  count={meta.count:3d}  "
              f"size={meta.size:6d} B  ser_type={meta.ser_type!r}")

    assert info.message_count == 10
    assert len(info.url_metas) == 2

    # No async_run / play() needed for read-only inspection.
    print("[OK] inspect-only metadata read")


# ===========================================================================
# Demo 7.  Synchronous cursor read (pull-based iteration)
# ===========================================================================

def demo_bag_cursor_read():
    """Tutorial: read a bag synchronously with the cursor instead of play().

    The cursor walks every stored frame in recorded order with no inter-frame
    delay and no background thread, so ``async_run()`` / ``play()`` are not
    needed.  ``open_cursor(cfg)`` applies ``filter_urls`` and the
    ``begin_time`` / ``end_time`` window; iterate with ``for frame in reader``
    (or call ``read_next()``), then tell a clean end apart from a backend read
    error via ``eof()`` and ``fail()``.
    """
    bag_path = _make_temp_bag_path("cursor.vdb")

    writer = _vlink.BagWriter.create(bag_path)
    assert writer is not None
    writer.async_run()

    for i in range(8):
        writer << _bag_frame(
            "intra://demo/cursor",
            "demo.Cursor",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            f"frame-{i}".encode(),
            timestamp=i * 10_000,
        )

    assert not writer.fail(), "streaming write latched a failure"
    assert writer.wait_for_idle(5000)
    writer.quit()
    assert writer.wait_for_quit(5000)
    writer.close()
    assert not writer.fail(), "writer finalization latched a failure"
    del writer

    # ---- Pull every frame via iteration --------------------------------
    reader = _vlink.BagReader.create(bag_path, read_only=True)
    assert reader is not None
    assert reader.open_cursor()

    payloads = [bytes(frame.data) for frame in reader]

    assert payloads == [f"frame-{i}".encode() for i in range(8)], f"got {payloads}"
    assert reader.eof()
    assert not reader.fail()
    print(f"  cursor iterated {len(payloads)} frames, eof={reader.eof()}, fail={reader.fail()}")

    # ---- Re-open with a begin window and read_next() -------------------
    cfg = _vlink.BagReader.Config()
    cfg.begin_time = 40
    assert reader.open_cursor(cfg)

    windowed = 0
    while True:
        frame = reader.read_next()
        if frame is None:
            break
        windowed += 1

    assert windowed == 4, f"got {windowed}"
    print(f"  windowed read returned {windowed} frames")
    print("[OK] cursor read")


# ===========================================================================
# Driver
# ===========================================================================

def main():
    """Run all demos in order.  Each demo prints a header so you can match
    the [OK] line to its tutorial."""
    _vlink.Logger.init("py_bag_demo")
    print(f"VLink Python Bag Tutorial - v{_vlink.VERSION}")
    print("=" * 60)

    print("\n--- Demo 1: simple record + replay ---")
    demo_bag_simple_record_replay()

    print("\n--- Demo 2: compression ---")
    demo_bag_with_compression()

    print("\n--- Demo 3: zero-copy types ---")
    demo_bag_zerocopy_record_replay()

    print("\n--- Demo 4: playback control (jump / rate) ---")
    demo_bag_playback_control()

    print("\n--- Demo 5: filter URLs ---")
    demo_bag_filter_urls()

    print("\n--- Demo 6: inspect-only metadata ---")
    demo_bag_inspect_only()

    print("\n--- Demo 7: synchronous cursor read ---")
    demo_bag_cursor_read()

    print("\n" + "=" * 60)
    print("ALL DEMOS FINISHED")


if __name__ == "__main__":
    main()

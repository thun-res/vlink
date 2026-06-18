# Copyright (C) 2026 by Thun Lu. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License").
"""
demo_vlink_communication.py
===========================

Tutorial-style demonstrations for the VLink Python communication API.

This file is the teaching counterpart of the test files under
``python_api/test/``.  Tests validate the binding contract; this file
**teaches the binding usage**.  Every function below is a self-contained,
top-to-bottom example that can be copied into a fresh script and run with
no modifications other than removing the ``demo_`` prefix.  The comments
explain WHY each step exists, not just what it does.

VLink exposes three communication models in Python:

    +---------------+--------------------------------------+----------------+
    | Model         | Pattern                              | Endpoint pair  |
    +===============+======================================+================+
    | Event model   | 1-to-many fan-out, fire-and-forget   | Publisher /    |
    |               |                                      | Subscriber     |
    +---------------+--------------------------------------+----------------+
    | Method model  | 1-to-1 request / response (RPC)      | Server /       |
    |               |                                      | Client         |
    +---------------+--------------------------------------+----------------+
    | Field model   | latest-value sync                    | Setter /       |
    |               |                                      | Getter         |
    +---------------+--------------------------------------+----------------+

Outline of this file
--------------------

  Section 1.  Event model with raw Bytes / vlink.Bytes
  Section 2.  Event model with schema-typed payloads (Protobuf / FlatBuffers)
  Section 3.  Event model with zero-copy typed containers
              (RawData, CameraFrame, PointCloud, OccupancyGrid, Tensor,
              ObjectArray, AudioFrame, ProxyData)
  Section 4.  Method model (Server / Client RPC)
  Section 5.  Field model (Setter / Getter latest-value sync)

All higher-level payload types travel through the ``vlink::Bytes`` codec
in Python.  ``Publisher<Bytes>`` / ``Subscriber<Bytes>`` /
``Server<Bytes,Bytes>`` / ``Client<Bytes,Bytes>`` / ``Setter<Bytes>`` /
``Getter<Bytes>`` are the only template instantiations exposed; users
serialise Python objects to bytes and parse them on the other side.

The minimum producer / consumer skeleton -- repeated in every Section 1-3
example -- is:

    1.  Build the Subscriber FIRST, register a ``listen(callback)``.
        Doing Sub-then-Pub guarantees the publisher's
        ``wait_for_subscribers`` call succeeds.

    2.  Build the Publisher and call ``wait_for_subscribers(timeout_ms)``
        so the publisher does not race ahead of the matched subscriber.

    3.  Convert the message to ``bytes`` / ``vlink.Bytes`` and publish.

    4.  Wait on a ``threading.Event`` until the callback has collected
        enough messages, then verify field equality.

    5.  Call ``deinit()`` on both endpoints.  GC cleanup also works, but
        explicit deinit makes shutdown ordering deterministic.

Server / Client follows a similar skeleton (Server first, then Client,
client invokes, server replies).  Setter / Getter is even simpler:
Getter registers a listener, Setter pushes new values.

Run
---
::

    python3 demo_vlink_communication.py

The Protobuf / FlatBuffers demos require ``google.protobuf`` and
``flatbuffers`` to be importable; otherwise they are skipped with a clear
``[SKIP]`` message.  All other demos require nothing beyond the VLink
nanobind module.
"""

import threading
import time

import _vlink_nanobind as _vlink  # type: ignore


# ===========================================================================
# Shared helpers
# ===========================================================================

def _make_node(cls, url, ser_type=""):
    """Construct and ``init()`` a Publisher / Subscriber / Server / Client.

    All node types share the same factory pattern: ``cls(url)`` then
    ``init()`` (mandatory).  An optional ``ser_type`` records the
    serialisation schema name so introspection tools (vlink-dump etc.) can
    decode payloads.
    """
    node = cls(url)

    if ser_type:
        node.set_ser_type(ser_type)

    node.init()
    return node


# ===========================================================================
# Section 1.  Raw Bytes pub/sub
# ===========================================================================

def demo_pubsub_bytes():
    """Tutorial: publish opaque ``bytes`` between two endpoints.

    This is the smallest possible VLink pub/sub example.  Use it when you
    just need to move a blob of bytes around -- application-defined
    serialisation, image dumps, log lines, etc.
    """
    received = []
    done = threading.Event()

    # --- Subscriber side ----------------------------------------------------
    # Create the Subscriber BEFORE the Publisher so the publisher's
    # `wait_for_subscribers` below can match it.
    sub = _make_node(_vlink.Subscriber, "intra://demo/bytes")

    def on_message(data):
        # `data` is a Python ``bytes`` object handed over by the binding.
        # The underlying memory may be a zero-copy view into the transport
        # buffer; copy it (e.g. ``bytes(data)``) if you need long-term
        # ownership.  Here we just record the value.
        received.append(data)

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    # --- Publisher side -----------------------------------------------------
    pub = _make_node(_vlink.Publisher, "intra://demo/bytes")
    pub.wait_for_subscribers(timeout_ms=2000)
    assert pub.has_subscribers(), "subscriber failed to match within 2 s"

    # --- Publish three messages --------------------------------------------
    for msg in (b"hello", b"world", b"vlink"):
        pub.publish(msg)

    # --- Wait for delivery and verify --------------------------------------
    assert done.wait(timeout=3.0), "timed out waiting for 3 messages"
    assert received == [b"hello", b"world", b"vlink"], f"got {received}"

    # --- Cleanup ------------------------------------------------------------
    pub.deinit()
    sub.deinit()
    print("[OK] Bytes pub/sub")


def demo_pubsub_vlink_bytes():
    """Tutorial: publish ``vlink.Bytes`` (the VLink-native byte buffer).

    ``vlink.Bytes`` is the C++ side of the wire and exposes shallow_copy /
    deep_copy / loan semantics that plain Python ``bytes`` cannot.  The
    binding accepts BOTH types on ``publish`` -- pick whichever fits the
    surrounding code.  Using ``vlink.Bytes`` matters when:

      * You want to wrap a buffer without copying (``shallow_copy``).
      * You want to reuse a pre-sized buffer (``create(n)``).
      * You are interoperating with a C++ component that produced the
        Bytes already.
    """
    received = []
    done = threading.Event()

    # --- Subscriber side ----------------------------------------------------
    sub = _make_node(_vlink.Subscriber, "intra://demo/vlink_bytes")

    def on_message(data):
        # `data` is a Python ``bytes`` regardless of what the publisher
        # passed.  The binding normalises Bytes -> Python bytes on receive.
        received.append(data)

        if len(received) >= 2:
            done.set()

    sub.listen(on_message)

    # --- Publisher side -----------------------------------------------------
    pub = _make_node(_vlink.Publisher, "intra://demo/vlink_bytes")
    pub.wait_for_subscribers(timeout_ms=2000)

    # --- Publish using vlink.Bytes ----------------------------------------
    # Construct a vlink.Bytes that owns its memory.  In real code you might
    # fill it via the C++ side or via shallow_copy(...) from a numpy buffer.
    owned = _vlink.Bytes.create(8)
    pub.publish(owned)

    # Construct from Python bytes -- equivalent to publishing the raw bytes
    # directly, but goes through the Bytes constructor for demo purposes.
    borrowed = _vlink.Bytes.from_bytes(b"vlink_bytes_demo")
    pub.publish(borrowed)

    # --- Wait, verify, cleanup ---------------------------------------------
    assert done.wait(timeout=3.0)
    assert len(received) == 2
    assert len(received[0]) == 8
    assert received[1] == b"vlink_bytes_demo", f"got {received[1]!r}"

    pub.deinit()
    sub.deinit()
    print("[OK] vlink.Bytes pub/sub")


# ===========================================================================
# Section 2.  Schema-typed payloads (Protobuf / FlatBuffers)
# ===========================================================================
#
# VLink does NOT serialise Protobuf or FlatBuffers messages for you.  The
# pub/sub layer transports opaque bytes; the schema is metadata declared
# via ``set_ser_type(name, schema_type)``.  Producers and consumers MUST
# share the schema generator output (``*_pb2.py`` for proto, ``*.py`` for
# fbs) out-of-band.
#
# The demos below try to import the schema-generated modules.  If they are
# absent, the demos print a ``[SKIP]`` message and return without failing.

def demo_pubsub_protobuf():
    """Tutorial: publish a Protobuf message.

    Pattern:

        1.  Set the publisher's schema type so introspection tools know
            what is on the wire::

                pub.set_ser_type("demo.proto.Message",
                                 _vlink.SchemaType.Protobuf)

        2.  In Python, serialise the message via ``SerializeToString()``.

        3.  Publish the resulting ``bytes``.

        4.  On the subscriber, parse the bytes via ``ParseFromString(...)``
            into a fresh Protobuf instance.

    This demo uses ``test/idl/test.proto`` (package ``pb``, message
    ``Message``).  Generate the Python binding with::

        protoc --python_out=. test/idl/test.proto
    """
    try:
        # Generated by protoc from test/idl/test.proto.  Build system places
        # this on the Python path when ENABLE_PYTHON_API and Protobuf
        # support are both on.  Adjust the import to match your repo layout.
        import test_pb2  # type: ignore
    except ImportError:
        print("[SKIP] Protobuf pub/sub (test_pb2.py not available -- "
              "run `protoc --python_out=. test/idl/test.proto`)")
        return

    received = []
    done = threading.Event()

    # --- Subscriber side ----------------------------------------------------
    sub = _make_node(_vlink.Subscriber, "intra://demo/protobuf",
                     ser_type="pb.Message")

    def on_message(payload):
        # The transport hands us raw bytes; parse them with the generated
        # Protobuf class.  ParseFromString throws DecodeError on malformed
        # input -- in production code you should catch that.
        msg = test_pb2.Message()
        msg.ParseFromString(payload)
        received.append((msg.type, msg.value))

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    # --- Publisher side -----------------------------------------------------
    pub = _vlink.Publisher("intra://demo/protobuf")
    # set_ser_type advertises BOTH the fully-qualified message name AND the
    # schema-type enum.  The latter lets dump tools pick the right decoder.
    pub.set_ser_type("pb.Message", _vlink.SchemaType.Protobuf)
    pub.init()
    pub.wait_for_subscribers(timeout_ms=2000)

    # --- Build, serialise, publish ----------------------------------------
    for type_id, value in [(1, "hello"), (2, "world"), (3, "vlink")]:
        msg = test_pb2.Message()
        msg.type = type_id
        msg.value = value

        # Serialise to bytes and publish.  No vlink-specific framing here;
        # the Protobuf bytes ARE the wire payload.
        pub.publish(msg.SerializeToString())

    # --- Wait, verify, cleanup ---------------------------------------------
    assert done.wait(timeout=3.0)
    assert received == [(1, "hello"), (2, "world"), (3, "vlink")], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] Protobuf pub/sub")


def demo_pubsub_flatbuffers():
    """Tutorial: publish a FlatBuffers message.

    Same idea as the Protobuf demo, but uses the ``flatbuffers`` Python
    runtime and the schema generated by ``flatc --python``::

        flatc --python -o python_api/ test/idl/test.fbs

    FlatBuffers gives you a Builder + serialise in one go; on the
    receiving side you wrap the bytes and read fields without parsing.
    """
    try:
        # Generated by `flatc --python` from test/idl/test.fbs.  Package
        # name comes from the `namespace fbs;` declaration in the .fbs.
        from fbs import Message as FbsMessage  # type: ignore
        import flatbuffers  # type: ignore
    except ImportError:
        print("[SKIP] FlatBuffers pub/sub (flatbuffers Python module or "
              "generated `fbs/Message.py` not available -- run "
              "`flatc --python test/idl/test.fbs`)")
        return

    received = []
    done = threading.Event()

    # --- Subscriber side ----------------------------------------------------
    sub = _make_node(_vlink.Subscriber, "intra://demo/flatbuffers",
                     ser_type="fbs.Message")

    def on_message(payload):
        # GetRootAs<Message> wraps the bytes without copying.  Field
        # accessors decode lazily on first call.
        msg = FbsMessage.Message.GetRootAs(payload, 0)
        # Convert bytes -> str: FlatBuffers stores strings as raw bytes.
        value_bytes = msg.Value() or b""
        received.append((msg.Type(), value_bytes.decode("utf-8")))

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    # --- Publisher side -----------------------------------------------------
    pub = _vlink.Publisher("intra://demo/flatbuffers")
    pub.set_ser_type("fbs.Message", _vlink.SchemaType.Flatbuffers)
    pub.init()
    pub.wait_for_subscribers(timeout_ms=2000)

    # --- Build, serialise, publish ----------------------------------------
    for type_id, value in [(1, "hello"), (2, "world"), (3, "vlink")]:
        # FlatBuffers builds the message tail-first.  The Builder owns a
        # growable buffer; FinishedBytes() returns the final wire bytes.
        builder = flatbuffers.Builder(1024)
        value_offset = builder.CreateString(value)

        FbsMessage.MessageStart(builder)
        FbsMessage.MessageAddType(builder, type_id)
        FbsMessage.MessageAddValue(builder, value_offset)
        msg_offset = FbsMessage.MessageEnd(builder)

        builder.Finish(msg_offset)
        wire = bytes(builder.Output())  # copy out the [head, head+size) view
        pub.publish(wire)

    # --- Wait, verify, cleanup ---------------------------------------------
    assert done.wait(timeout=3.0)
    assert received == [(1, "hello"), (2, "world"), (3, "vlink")], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] FlatBuffers pub/sub")


# ===========================================================================
# Section 3.  Zero-copy typed containers
# ===========================================================================
#
# Each VLink zero-copy type provides ``to_bytes()`` and ``from_bytes()``
# that wrap a magic-number envelope around the struct snapshot + payload.
# Sending one over pub/sub is exactly the same pattern as Protobuf /
# FlatBuffers -- just use to_bytes()/from_bytes() instead of
# SerializeToString()/ParseFromString().
#
# Tip: ``from_bytes()`` returns False if the magic envelope is corrupt;
# always check the return value before reading fields.


def demo_pubsub_raw_data():
    """Tutorial: publish ``RawData`` -- the simplest zero-copy container.

    ``RawData`` = ``Header`` + variable-length opaque buffer.  Reach for it
    when you have an application-defined payload and want sequence numbers
    / timestamps "for free" via the Header.
    """
    received = []
    done = threading.Event()

    # --- Subscriber side ----------------------------------------------------
    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/raw_data")

    def on_message(payload):
        rd = _vlink.RawData()
        # from_bytes validates the magic envelope and zero-copies the
        # payload view from the wire buffer.  Returns False on corruption.
        assert rd.from_bytes(payload)

        received.append((rd.header.seq, rd.size()))

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    # --- Publisher side -----------------------------------------------------
    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/raw_data")
    pub.wait_for_subscribers(timeout_ms=2000)

    # --- Publish three RawData payloads of different sizes ----------------
    for seq, size in enumerate((64, 128, 256), start=1):
        rd = _vlink.RawData()
        # Header is a public POD; set fields directly.
        rd.header.seq = seq
        # Allocate `size` uninitialised bytes for the payload.
        assert rd.create(size)
        # Serialise -> wire bytes -> publish.
        pub.publish(rd.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [(1, 64), (2, 128), (3, 256)], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] RawData pub/sub")


def demo_pubsub_camera_frame():
    """Tutorial: publish ``CameraFrame`` -- pixel data with format metadata."""
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/camera_frame")

    def on_message(payload):
        frame = _vlink.CameraFrame()
        assert frame.from_bytes(payload)
        received.append({
            "seq": frame.header.seq,
            "width": frame.width(),
            "height": frame.height(),
            "format": frame.format(),
        })

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/camera_frame")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq, (w, h) in enumerate([(320, 240), (640, 480), (1280, 720)], start=1):
        frame = _vlink.CameraFrame()
        frame.header.seq = seq
        frame.set_width(w)
        frame.set_height(h)
        # NV12 = canonical automotive pixel format: Y plane + interleaved UV.
        frame.set_format(_vlink.CameraFrame.Format.Nv12)
        frame.set_stream(_vlink.CameraFrame.Stream.I)
        # NV12 = 1.5 bytes per pixel.
        assert frame.create(w * h * 3 // 2)
        pub.publish(frame.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        {"seq": 1, "width": 320,  "height": 240,
         "format": _vlink.CameraFrame.Format.Nv12},
        {"seq": 2, "width": 640,  "height": 480,
         "format": _vlink.CameraFrame.Format.Nv12},
        {"seq": 3, "width": 1280, "height": 720,
         "format": _vlink.CameraFrame.Format.Nv12},
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] CameraFrame pub/sub")


def demo_pubsub_point_cloud():
    """Tutorial: publish ``PointCloud`` -- LiDAR-style schema-aware container.

    ``PointCloud`` stores N points sharing a custom schema (field name list
    + per-field type).  The schema is encoded in TWO 64-bit integers passed
    to ``create(count, size_num, type_num, names)``.  This demo uses the
    simplest valid schema: three float fields ``x,y,z``.
    """
    received = []
    done = threading.Event()

    # Schema encoding: nibble-packed, highest nibble = first field.
    #   size_num: per-field byte size.  3 floats -> (4<<8)|(4<<4)|4 = 0x444.
    #   type_num: per-field DataType.  kFloatType (10) -> 0xAAA.
    size_num = (4 << 8) | (4 << 4) | 4
    type_num = (10 << 8) | (10 << 4) | 10

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/point_cloud")

    def on_message(payload):
        pc = _vlink.PointCloud()
        assert pc.from_bytes(payload)
        # get_value_v3f returns a Vector3f copy of the i-th point.
        v0 = pc.get_value_v3f(0)
        received.append((pc.header.seq, pc.size(), v0.x, v0.y, v0.z))

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/point_cloud")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq in (1, 2, 3):
        pc = _vlink.PointCloud()
        pc.header.seq = seq
        # Reserve 16 point slots with the float-xyz schema.
        assert pc.create(16, size_num, type_num, "x,y,z")
        # Append two points; coordinates depend on seq to make each frame
        # distinguishable on the consumer side.
        assert pc.push_value_v3f(float(seq), float(seq + 1), float(seq + 2))
        assert pc.push_value_v3f(float(seq * 10), float(seq * 20), float(seq * 30))
        pub.publish(pc.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        (1, 2, 1.0, 2.0, 3.0),
        (2, 2, 2.0, 3.0, 4.0),
        (3, 2, 3.0, 4.0, 5.0),
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] PointCloud pub/sub")


def demo_pubsub_proxy_data():
    """Tutorial: publish ``ProxyData`` -- VLink's routing envelope.

    Bundles a raw payload with routing metadata (URL, schema name,
    hostname).  Used internally by the VLink proxy layer; rarely needed
    by applications, but included for completeness.
    """
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/proxy_data")

    def on_message(payload):
        pd = _vlink.ProxyData()
        assert pd.from_bytes(payload)
        received.append((pd.url(), pd.ser(), pd.hostname()))

        if len(received) >= 2:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/proxy_data")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq in (1, 2):
        pd = _vlink.ProxyData()
        raw = _vlink.Bytes.create(8 + seq)
        # `create` populates url + schema name + schema type enum + hostname
        # in one call.  All four are mandatory before to_bytes().
        pd.create(raw, f"intra://demo/proxy/{seq}", "demo.RawBytes",
                  int(_vlink.SchemaType.Raw), "host01")
        pub.publish(pd.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        ("intra://demo/proxy/1", "demo.RawBytes", "host01"),
        ("intra://demo/proxy/2", "demo.RawBytes", "host01"),
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] ProxyData pub/sub")


def demo_pubsub_occupancy_grid():
    """Tutorial: publish ``OccupancyGrid`` -- 2-D occupancy / cost map.

    Used by SLAM front-ends, local planners, and costmaps.  Cells occupy
    1, 2, or 4 bytes depending on ``CellType``.
    """
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/occupancy_grid")

    def on_message(payload):
        og = _vlink.OccupancyGrid()
        assert og.from_bytes(payload)
        received.append((og.header.seq, og.width(), og.height(), og.map_id()))

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/occupancy_grid")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq, (w, h) in enumerate([(40, 40), (80, 60), (100, 100)], start=1):
        og = _vlink.OccupancyGrid()
        og.header.seq = seq

        # Geometry: width/height in cells, resolution in metres per cell.
        og.set_width(w)
        og.set_height(h)
        og.set_resolution(0.05)
        og.set_origin_x(-float(w) * 0.5 * 0.05)
        og.set_origin_y(-float(h) * 0.5 * 0.05)

        # Cell encoding: ROS-style int8 (-1 unknown, 0 free, 100 occupied).
        og.set_cell_type(_vlink.OccupancyGrid.CellType.Int8)
        og.set_default_value(-1)
        og.set_map_id(f"map_{seq}")

        # `create` allocates ``width * height * cell_size`` bytes; for Int8
        # cell_size == 1 so width * height is enough.
        assert og.create(w * h)
        pub.publish(og.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        (1, 40,  40,  "map_1"),
        (2, 80,  60,  "map_2"),
        (3, 100, 100, "map_3"),
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] OccupancyGrid pub/sub")


def demo_pubsub_tensor():
    """Tutorial: publish ``Tensor`` -- N-D tensor (neural-network I/O).

    ORDER MATTERS: call ``set_dtype()`` BEFORE ``set_shape()`` so the
    per-element size is cached when set_shape derives strides.
    """
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/tensor")

    def on_message(payload):
        t = _vlink.Tensor()
        assert t.from_bytes(payload)
        # rank / num_elements are cached during set_shape().  They are also
        # re-validated by operator<< on deserialisation (rank is clamped
        # to kMaxRank, element_size is re-derived from dtype).
        received.append({
            "seq": t.header.seq,
            "rank": t.rank(),
            "num_elements": int(t.num_elements()),
            "dtype": t.dtype(),
        })

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/tensor")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq, shape in enumerate([[1, 3, 32, 32], [1, 16, 8, 8], [2, 64]],
                                start=1):
        t = _vlink.Tensor()
        t.header.seq = seq

        # Descriptive metadata -- helps consumers route on producer intent.
        t.set_name("feature_map")
        t.set_layout("NCHW" if len(shape) == 4 else "ND")

        # set_dtype MUST come before set_shape (see docstring above).
        t.set_dtype(_vlink.Tensor.DataType.Float32)
        t.set_shape(shape)  # computes strides + num_elements

        # Allocate num_elements * sizeof(float) bytes for the payload.
        total = 1
        for d in shape:
            total *= d
        assert t.create(total * 4)

        pub.publish(t.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        {"seq": 1, "rank": 4, "num_elements": 1 * 3 * 32 * 32,
         "dtype": _vlink.Tensor.DataType.Float32},
        {"seq": 2, "rank": 4, "num_elements": 1 * 16 * 8 * 8,
         "dtype": _vlink.Tensor.DataType.Float32},
        {"seq": 3, "rank": 2, "num_elements": 2 * 64,
         "dtype": _vlink.Tensor.DataType.Float32},
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] Tensor pub/sub")


def demo_pubsub_object_array():
    """Tutorial: publish ``ObjectArray`` -- 3-D detection / tracking results.

    Variable-length array of fixed-size ``Object`` records.  Each ``Object``
    is a public POD: assign fields directly, no setter methods.
    """
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/object_array")

    def on_message(payload):
        arr = _vlink.ObjectArray()
        assert arr.from_bytes(payload)
        # objects(i) returns a copy of the i-th Object (Python value
        # semantics).  Iterate up to arr.count().
        first = arr.objects(0)
        received.append({
            "seq": arr.header.seq,
            "count": arr.count(),
            "source_id": arr.source_id(),
            "label": first.label,
            "class_id": first.class_id,
            "track_id": first.track_id,
        })

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/object_array")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq, label in enumerate(("car", "pedestrian", "cyclist"), start=1):
        arr = _vlink.ObjectArray()
        arr.header.seq = seq

        # Pre-allocate room for 8 Objects (count_ stays 0 until push_value).
        assert arr.create(8)
        arr.set_source_id(f"fusion_{seq}")

        # Build one Object: ALL fields are public; no setter methods.
        obj = _vlink.ObjectArray.Object()
        obj.label = label                                 # str -> char[32]
        obj.position = [float(seq), float(seq) * 2.0, 0.0]
        obj.size = [4.5, 1.8, 1.6]                        # length, width, height
        obj.yaw = 0.1 * float(seq)
        obj.velocity = [float(seq) * 5.0, 0.0, 0.0]
        obj.class_id = seq
        obj.track_id = seq + 100
        obj.motion_state = _vlink.ObjectArray.MotionState.Moving
        obj.source_type = _vlink.ObjectArray.SourceType.Fusion

        # Append to the array (advances arr.count() by 1).
        assert arr.push_value(obj)

        pub.publish(arr.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        {"seq": 1, "count": 1, "source_id": "fusion_1",
         "label": "car",        "class_id": 1, "track_id": 101},
        {"seq": 2, "count": 1, "source_id": "fusion_2",
         "label": "pedestrian", "class_id": 2, "track_id": 102},
        {"seq": 3, "count": 1, "source_id": "fusion_3",
         "label": "cyclist",    "class_id": 3, "track_id": 103},
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] ObjectArray pub/sub")


def demo_pubsub_audio_frame():
    """Tutorial: publish ``AudioFrame`` -- PCM or compressed audio.

    20 ms of 48 kHz stereo PCM_S16 (960 samples per channel) is the
    canonical "Opus packet equivalent" used by infotainment and voice
    pipelines.
    """
    received = []
    done = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://demo/zerocopy/audio_frame")

    def on_message(payload):
        af = _vlink.AudioFrame()
        assert af.from_bytes(payload)
        received.append({
            "seq": af.header.seq,
            "sample_rate": af.sample_rate(),
            "num_channels": af.num_channels(),
            "format": af.format(),
            "codec": af.codec(),
        })

        if len(received) >= 3:
            done.set()

    sub.listen(on_message)

    pub = _make_node(_vlink.Publisher, "intra://demo/zerocopy/audio_frame")
    pub.wait_for_subscribers(timeout_ms=2000)

    for seq, sr in enumerate((16000, 44100, 48000), start=1):
        af = _vlink.AudioFrame()
        af.header.seq = seq

        # Sample-stream metadata.
        af.set_sample_rate(sr)
        af.set_num_channels(2)             # stereo
        af.set_num_samples(960)            # 20 ms @ 48 kHz
        af.set_bit_depth(16)
        af.set_format(_vlink.AudioFrame.Format.PcmS16)
        af.set_layout(_vlink.AudioFrame.Layout.Interleaved)

        # Descriptive fields.
        af.set_codec("PCM")
        af.set_language("en")

        # PCM_S16 stereo: 2 channels * 2 bytes/sample * 960 samples.
        assert af.create(960 * 2 * 2)
        pub.publish(af.to_bytes())

    assert done.wait(timeout=3.0)
    assert received == [
        {"seq": 1, "sample_rate": 16000, "num_channels": 2,
         "format": _vlink.AudioFrame.Format.PcmS16, "codec": "PCM"},
        {"seq": 2, "sample_rate": 44100, "num_channels": 2,
         "format": _vlink.AudioFrame.Format.PcmS16, "codec": "PCM"},
        {"seq": 3, "sample_rate": 48000, "num_channels": 2,
         "format": _vlink.AudioFrame.Format.PcmS16, "codec": "PCM"},
    ], f"got {received}"

    pub.deinit()
    sub.deinit()
    print("[OK] AudioFrame pub/sub")


# ===========================================================================
# Section 4.  Method model -- Server / Client (RPC)
# ===========================================================================
#
# Where Publisher / Subscriber is 1-to-many fire-and-forget, Server /
# Client is 1-to-1 request / response.  The client BLOCKS until the
# server replies (or times out).  Use it for control / config / query
# style interactions where each call expects an answer.
#
# Skeleton:
#
#     1.  Build the Server first, register ``listen(handler)``.  The
#         handler receives a Bytes request and MUST return a Bytes
#         (or bytes / bytearray) response.
#     2.  Build the Client and call ``wait_for_connected(timeout_ms)``
#         so the client does not race ahead of the matched server.
#     3.  Call ``client.invoke(request_bytes)`` -- returns response bytes
#         synchronously; or ``invoke_async(req, callback)`` for non-
#         blocking calls.
#     4.  Deinit both endpoints.


def demo_rpc_sync():
    """Tutorial: synchronous RPC with ``invoke()``.

    The simplest RPC pattern: client sends bytes, server replies bytes,
    client blocks until response arrives.  Equivalent to ``requests.post``
    in feel, but over VLink.
    """
    # --- Server side ----------------------------------------------------
    # Server is created first so the client can match it.
    server = _make_node(_vlink.Server, "intra://demo/rpc/echo")

    def handle_request(request):
        # The handler runs on the server's internal worker thread.  The
        # return value (any bytes-like) becomes the response payload.
        # If the handler raises, the client sees an error.
        return b"echo:" + bytes(request)

    server.listen(handle_request)

    # --- Client side ----------------------------------------------------
    client = _make_node(_vlink.Client, "intra://demo/rpc/echo")

    # wait_for_connected blocks until at least one matching server is up.
    client.wait_for_connected(timeout_ms=2000)
    assert client.is_connected(), "client failed to match server"

    # --- Two synchronous calls ------------------------------------------
    response_a = client.invoke(b"hello")
    response_b = client.invoke(b"vlink")

    assert response_a == b"echo:hello", f"got {response_a!r}"
    assert response_b == b"echo:vlink", f"got {response_b!r}"

    # --- Cleanup --------------------------------------------------------
    client.deinit()
    server.deinit()
    print("[OK] RPC sync")


def demo_rpc_async():
    """Tutorial: asynchronous RPC with ``invoke_async(req, callback)``.

    ``invoke_async`` returns immediately; the response is delivered on
    the client's internal thread by invoking ``callback(response_bytes)``.
    Use when the caller cannot afford to block on the request -- e.g.
    inside an event loop or when issuing many calls in parallel.
    """
    server = _make_node(_vlink.Server, "intra://demo/rpc/uppercase")

    # Server simulates a small amount of work then returns an uppercased
    # version of the request.  The 50 ms sleep matters for the demo: the
    # async client should NOT block this thread while waiting.
    def handle_request(request):
        time.sleep(0.05)
        return bytes(request).upper()

    server.listen(handle_request)

    client = _make_node(_vlink.Client, "intra://demo/rpc/uppercase")
    client.wait_for_connected(timeout_ms=2000)

    # Fire three requests in quick succession.  Each completes
    # independently on the client's worker thread.
    received = {}
    done = threading.Event()

    def make_callback(tag):
        def cb(response):
            # Callback runs on the client's worker thread; copy the bytes
            # out of the borrowed view if you want to keep them.
            received[tag] = bytes(response)

            if len(received) >= 3:
                done.set()
        return cb

    client.invoke_async(b"alpha", make_callback("alpha"))
    client.invoke_async(b"beta",  make_callback("beta"))
    client.invoke_async(b"gamma", make_callback("gamma"))

    # All three replies should arrive within ~ 50 ms + overhead.
    assert done.wait(timeout=2.0)
    assert received == {"alpha": b"ALPHA",
                        "beta":  b"BETA",
                        "gamma": b"GAMMA"}, f"got {received}"

    client.deinit()
    server.deinit()
    print("[OK] RPC async")


def demo_rpc_with_zerocopy():
    """Tutorial: RPC carrying typed zero-copy payloads.

    The Server's handler receives the raw request bytes and returns raw
    response bytes -- same encoding rules as Pub/Sub.  Here the request
    is a serialised ``Tensor`` describing an input image; the server
    "runs inference" (a stub that doubles every element) and returns a
    ``Tensor`` of the same shape.  This is the canonical pattern for
    cross-process inference / batch processing.
    """
    server = _make_node(_vlink.Server, "intra://demo/rpc/tensor_eval")

    def handle_request(request):
        # Decode the request as a Tensor.
        in_tensor = _vlink.Tensor()
        ok = in_tensor.from_bytes(request)
        assert ok, "server: request did not parse as a Tensor"

        # Build an output tensor with identical shape / dtype.  In real
        # code you would invoke a model here; we simply construct a new
        # tensor of the same size to demonstrate the wire round-trip.
        out_tensor = _vlink.Tensor()
        out_tensor.set_name("output")
        out_tensor.set_layout(in_tensor.layout())
        out_tensor.set_dtype(in_tensor.dtype())
        out_tensor.set_shape(list(in_tensor.shape()))
        assert out_tensor.create(int(out_tensor.num_elements()) *
                                 int(out_tensor.element_size()))

        # Return the serialised tensor as the response payload.
        return out_tensor.to_bytes()

    server.listen(handle_request)

    client = _make_node(_vlink.Client, "intra://demo/rpc/tensor_eval")
    client.wait_for_connected(timeout_ms=2000)

    # Build the input tensor.
    in_tensor = _vlink.Tensor()
    in_tensor.set_name("input")
    in_tensor.set_layout("NCHW")
    in_tensor.set_dtype(_vlink.Tensor.DataType.Float32)
    in_tensor.set_shape([1, 3, 16, 16])
    assert in_tensor.create(1 * 3 * 16 * 16 * 4)

    # Synchronous invoke -- response bytes come back inline.
    response_bytes = client.invoke(in_tensor.to_bytes())

    # Decode the response as a Tensor.
    out_tensor = _vlink.Tensor()
    assert out_tensor.from_bytes(response_bytes), "client: response did not parse"
    assert out_tensor.dtype() == _vlink.Tensor.DataType.Float32
    assert int(out_tensor.num_elements()) == 1 * 3 * 16 * 16

    client.deinit()
    server.deinit()
    print("[OK] RPC carrying Tensor payload")


# ===========================================================================
# Section 5.  Field model -- Setter / Getter (latest-value sync)
# ===========================================================================
#
# Field is for state that has a "latest value" -- vehicle speed,
# parameter values, configuration knobs, etc.  Unlike Publisher /
# Subscriber, the Field model GUARANTEES the late-joiner sees the
# most recent value as soon as it attaches, even if no Setter has run
# since.  Conceptually: a key/value store backed by VLink transport.
#
# Two reading patterns:
#   * push:  Getter.listen(callback) -- callback fires on every change
#   * pull:  Getter.get()            -- returns the latest known value
#
# Either or both can be used; the binding handles caching.


def demo_field_push():
    """Tutorial: push-style Field consumer via ``Getter.listen``.

    The Getter is created first, registers a change callback, and the
    Setter then publishes new values.  Useful when the consumer wants to
    react to every change.
    """
    received = []
    event = threading.Event()

    # --- Getter (consumer) side -----------------------------------------
    getter = _make_node(_vlink.Getter, "intra://demo/field/speed")

    def on_change(data):
        received.append(bytes(data))

        # First three values trigger the wait.
        if len(received) >= 3:
            event.set()

    getter.listen(on_change)

    # --- Setter (producer) side -----------------------------------------
    setter = _make_node(_vlink.Setter, "intra://demo/field/speed")

    # Brief sleep so discovery / matching completes.  Production code
    # typically polls ``getter.has_setters()`` instead.
    time.sleep(0.05)

    # Three state updates simulating vehicle speed changing over time.
    setter.set(b"10.0")
    setter.set(b"22.5")
    setter.set(b"40.0")

    assert event.wait(timeout=2.0)
    assert received == [b"10.0", b"22.5", b"40.0"], f"got {received}"

    setter.deinit()
    getter.deinit()
    print("[OK] Field model (push via Getter.listen)")


def demo_field_pull():
    """Tutorial: pull-style Field consumer via ``Getter.get()``.

    The Setter publishes a value; the Getter polls ``get()`` until it
    sees the latest value.  Useful for late-joiner consumers or anywhere
    a callback is awkward (e.g. inside a sync function).

    Note: Field model is "latest-value cached", so any Getter started
    AFTER the Setter has published will still receive the most recent
    value -- there is no need to start the Getter first.
    """
    setter = _make_node(_vlink.Setter, "intra://demo/field/config")
    setter.set(b"max_speed=60")

    # Tiny delay so the value lands before the Getter attaches.
    time.sleep(0.05)

    # Getter attaches AFTER the Setter -- still sees the cached value.
    getter = _make_node(_vlink.Getter, "intra://demo/field/config")

    # Poll up to 2 s for the value to become visible.
    deadline = time.time() + 2.0
    value = None
    while time.time() < deadline:
        value = getter.get()
        if value == b"max_speed=60":
            break

        time.sleep(0.01)

    assert value == b"max_speed=60", f"got {value!r}"

    # Update the value and pull again.
    setter.set(b"max_speed=80")
    time.sleep(0.05)
    assert getter.get() == b"max_speed=80"

    setter.deinit()
    getter.deinit()
    print("[OK] Field model (pull via Getter.get)")


# ===========================================================================
# Driver
# ===========================================================================

def main():
    """Run all demos in order.

    Demos share the ``intra://`` transport but use distinct topic paths,
    so they cannot interfere with each other.  Each demo cleans up its
    endpoints before returning.
    """
    _vlink.Logger.init("py_demo")
    print(f"VLink Python Communication Tutorial - v{_vlink.VERSION}")
    print("=" * 60)

    print("\n--- Section 1: Event model with raw Bytes ---")
    demo_pubsub_bytes()
    demo_pubsub_vlink_bytes()

    print("\n--- Section 2: Event model with schema-typed payloads ---")
    demo_pubsub_protobuf()
    demo_pubsub_flatbuffers()

    print("\n--- Section 3: Event model with zero-copy typed containers ---")
    demo_pubsub_raw_data()
    demo_pubsub_camera_frame()
    demo_pubsub_point_cloud()
    demo_pubsub_proxy_data()
    demo_pubsub_occupancy_grid()
    demo_pubsub_tensor()
    demo_pubsub_object_array()
    demo_pubsub_audio_frame()

    print("\n--- Section 4: Method model (Server / Client RPC) ---")
    demo_rpc_sync()
    demo_rpc_async()
    demo_rpc_with_zerocopy()

    print("\n--- Section 5: Field model (Setter / Getter) ---")
    demo_field_push()
    demo_field_pull()

    print("\n" + "=" * 60)
    print("ALL DEMOS FINISHED")


if __name__ == "__main__":
    main()

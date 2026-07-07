#!/usr/bin/env python3
"""
VLink Python bindings self-test.

Usage:
    cd build_python/output/lib
    LD_LIBRARY_PATH=. python3 ../../../python_api/test_vlink.py
"""

import os
import struct
import threading
import time

os.environ["VLINK_DISCOVER_DISABLE"] = "1"

import vlink as _vlink  # type: ignore

assert getattr(_vlink, "__backend__", None) == "nanobind"


def _make_node(cls, url, ser_type=""):
    node = cls(url, ser_type=ser_type, auto_init=False)
    node.set_discovery_enabled(False)
    node.init()
    return node


def test_bytes():
    """Test Bytes class."""
    b = _vlink.Bytes.from_bytes(b"hello world")
    assert b.to_bytes() == b"hello world"
    assert len(b) == 11
    assert bool(b)
    assert not bool(_vlink.Bytes())
    assert b.to_string() == "hello world"

    # Base64
    encoded = _vlink.Bytes.encode_to_base64(b)
    decoded = _vlink.Bytes.decode_from_base64(encoded)
    assert decoded.to_bytes() == b"hello world"

    # CRC-32 (CRC-32/ISO-HDLC)
    crc32 = _vlink.Bytes.get_crc_32(b)
    assert isinstance(crc32, int) and crc32 > 0

    # CRC-64 (CRC-64/ECMA-182)
    crc64 = _vlink.Bytes.get_crc_64(b)
    assert isinstance(crc64, int) and crc64 > 0

    # Compression
    big = _vlink.Bytes.from_bytes(b"A" * 10000)
    compressed = _vlink.Bytes.compress(big)
    decompressed = _vlink.Bytes.uncompress(compressed)
    assert decompressed.to_bytes() == b"A" * 10000
    assert compressed.size() < big.size()

    # Buffer protocol
    mv = memoryview(_vlink.Bytes.from_bytes(b"\x01\x02\x03"))
    assert mv[0] == 1 and mv[2] == 3

    # Bytes-like input support
    ba = bytearray(b"abc")
    from_bytearray = _vlink.Bytes.from_bytes(ba)
    assert from_bytearray.to_bytes() == b"abc"

    print("[PASS] Bytes")


def test_pubsub():
    """Test Publisher / Subscriber (Event model)."""
    received = []
    event = threading.Event()

    sub = _make_node(_vlink.Subscriber, "intra://py_test/pubsub")
    sub.listen(lambda data: (received.append(data), event.set() if len(received) >= 3 else None))

    pub = _make_node(_vlink.Publisher, "intra://py_test/pubsub")
    pub.wait_for_subscribers(timeout_ms=2000)
    assert pub.has_subscribers()

    pub.publish(b"a")
    pub.publish(b"b")
    pub.publish(b"c")
    event.wait(timeout=2.0)

    assert received == [b"a", b"b", b"c"], f"Got: {received}"
    pub.deinit()
    sub.deinit()
    print("[PASS] Publisher/Subscriber")


def test_rpc():
    """Test Server / Client (Method model)."""
    srv = _make_node(_vlink.Server, "intra://py_test/rpc")
    srv.listen(lambda req: b"re:" + req)

    cli = _make_node(_vlink.Client, "intra://py_test/rpc")
    cli.wait_for_connected(timeout_ms=2000)
    assert cli.is_connected()

    assert cli.invoke(b"hello") == b"re:hello"
    assert cli.invoke(b"world") == b"re:world"

    # Async invoke
    result = [None]
    ev = threading.Event()
    cli.invoke_async(b"async", lambda r: (result.__setitem__(0, r), ev.set()))
    ev.wait(timeout=2.0)
    assert result[0] == b"re:async"

    cli.deinit()
    srv.deinit()
    print("[PASS] Server/Client RPC")


def test_field():
    """Test Setter / Getter (Field model)."""
    values = []
    ev = threading.Event()

    getter = _make_node(_vlink.Getter, "intra://py_test/field")
    getter.listen(lambda data: (values.append(data), ev.set()))

    setter = _make_node(_vlink.Setter, "intra://py_test/field")
    time.sleep(0.05)

    setter.set(b"v1")
    ev.wait(timeout=2.0)
    assert values[-1] == b"v1"

    ev.clear()
    setter.set(b"v2")
    ev.wait(timeout=2.0)
    assert values[-1] == b"v2"

    # Polling
    deadline = time.time() + 2.0
    val = None
    while time.time() < deadline:
        val = getter.get()
        if val == b"v2":
            break
        time.sleep(0.01)
    assert val == b"v2"

    setter.deinit()
    getter.deinit()
    print("[PASS] Setter/Getter")


def test_message_loop():
    """Test MessageLoop + Timer."""
    loop = _vlink.MessageLoop()
    loop.async_run()
    assert loop.is_running()

    done = threading.Event()
    loop.post_task(done.set)
    done.wait(timeout=2.0)
    assert done.is_set()

    # Timer
    ticks = [0]
    timer_done = threading.Event()
    timer = _vlink.Timer(loop)
    timer.set_interval(30)
    timer.set_loop_count(3)

    def on_tick():
        ticks[0] += 1
        if ticks[0] >= 3:
            timer_done.set()

    timer.start(on_tick)
    timer_done.wait(timeout=2.0)
    timer.stop()
    assert ticks[0] >= 3

    loop.quit()
    loop.wait_for_quit(timeout_ms=2000)
    print("[PASS] MessageLoop + Timer")


def test_thread_pool():
    """Test ThreadPool."""
    pool = _vlink.ThreadPool(2)
    count = [0]
    ev = threading.Event()
    lock = threading.Lock()

    def task():
        with lock:
            count[0] += 1
            if count[0] >= 4:
                ev.set()

    for _ in range(4):
        pool.post_task(task)
    ev.wait(timeout=2.0)
    assert count[0] >= 4
    pool.shutdown()
    print("[PASS] ThreadPool")


def test_utils():
    """Test utility functions."""
    assert _vlink.utils.get_host_name()
    assert _vlink.utils.get_pid() > 0
    assert _vlink.utils.get_tmp_dir()
    assert _vlink.helpers.has_startwith("hello", "hel")
    assert _vlink.helpers.has_endwith("hello", "llo")
    assert _vlink.helpers.trim_string("  x  ") == "x"
    assert _vlink.helpers.trim_string_view("  x  ") == "x"
    print("[PASS] Utils + Helpers")


def test_quantize():
    """Test base quantize helpers."""
    stored = _vlink.quantize.encode_int16(-10.0, 10.0, 1.25)
    assert stored == 4096

    value = _vlink.quantize.decode_int16(-10.0, 10.0, stored)
    assert abs(value - 1.25) < 1e-3

    stored_extent = _vlink.quantize.encode_int16(10.0, 1.25)
    assert stored_extent == stored
    value_extent = _vlink.quantize.decode_int16(10.0, stored_extent)
    assert abs(value_extent - 1.25) < 1e-3

    assert _vlink.quantize.encode_int16(-10.0, 10.0, 100.0) == 32767
    assert _vlink.quantize.encode_int16(-10.0, 10.0, -100.0) == -32768
    saturated_min = _vlink.quantize.decode_int16(-10.0, 10.0, -32768)
    assert -10.001 < saturated_min < -10.0
    assert _vlink.quantize.encode_int16(10.0, 10.0, 1.0) == 0
    assert _vlink.quantize.encode_int16(0.0, 1.0) == 0

    print("[PASS] Quantize")


def test_qos():
    """Test QoS profiles."""
    qos_map = _vlink.QosProfile.get_available_qos_map()
    assert len(qos_map) > 0
    assert "event" in qos_map
    assert "method" in qos_map
    assert "field" in qos_map
    print(f"[PASS] QosProfile ({len(qos_map)} profiles)")


def test_uuid():
    """Test Uuid class (RFC 4122 128-bit UUID + v4 random + random_hex/random_bytes)."""

    # ---- constants ----
    assert _vlink.Uuid.BYTE_SIZE == 16
    assert _vlink.Uuid.STRING_SIZE == 36

    # ---- default-constructed = nil ----
    nil = _vlink.Uuid()
    assert nil.is_nil()
    assert nil.to_string() == "00000000-0000-0000-0000-000000000000"
    assert nil.to_compact_string() == "00000000000000000000000000000000"
    assert nil.variant() == _vlink.Uuid.Variant.Ncs
    assert nil.version() == _vlink.Uuid.Version.None_

    # ---- construct from bytes (16-byte payload) ----
    raw = bytes.fromhex("47ac10b858cc4a3c8c5b0e778899aabb")
    fixed = _vlink.Uuid(raw)
    assert not fixed.is_nil()
    assert fixed.bytes() == raw
    assert fixed.to_string() == "47ac10b8-58cc-4a3c-8c5b-0e778899aabb"
    assert fixed.to_compact_string() == "47ac10b858cc4a3c8c5b0e778899aabb"
    assert str(fixed) == fixed.to_string()
    assert "47ac10b8" in repr(fixed)

    # wrong-size payload -> ValueError
    try:
        _vlink.Uuid(b"\x00" * 8)
        assert False, "expected ValueError for 8-byte payload"
    except ValueError:
        pass

    # ---- generate_random produces v4 RFC ----
    a = _vlink.Uuid.generate_random()
    b = _vlink.Uuid.generate_random()
    assert not a.is_nil()
    assert a != b
    assert a.variant() == _vlink.Uuid.Variant.Rfc
    assert a.version() == _vlink.Uuid.Version.RandomBased

    # ---- comparison + hash ----
    a_copy = _vlink.Uuid(a.bytes())
    assert a == a_copy
    assert hash(a) == hash(a_copy)
    assert (a < b) or (b < a)

    # usable as dict key
    table = {a: 1, b: 2}
    assert table[a] == 1 and table[b] == 2

    # ---- parsing ----
    canonical = "47ac10b8-58cc-4a3c-8c5b-0e778899aabb"
    parsed = _vlink.Uuid.from_string(canonical)
    assert parsed is not None and parsed.to_string() == canonical

    # mixed-case
    assert _vlink.Uuid.from_string("47AC10B8-58CC-4A3C-8C5B-0E778899AABB").to_string() == canonical

    # braced
    assert _vlink.Uuid.from_string("{" + canonical + "}").to_string() == canonical

    # compact 32-char
    compact = canonical.replace("-", "")
    assert _vlink.Uuid.from_string(compact).to_string() == canonical

    # malformed -> None
    assert _vlink.Uuid.from_string("not-a-uuid") is None
    assert _vlink.Uuid.from_string("") is None
    assert _vlink.Uuid.from_string("{abc") is None

    # ---- is_valid ----
    assert _vlink.Uuid.is_valid(canonical)
    assert _vlink.Uuid.is_valid("{" + canonical + "}")
    assert _vlink.Uuid.is_valid(compact)
    assert not _vlink.Uuid.is_valid("")
    assert not _vlink.Uuid.is_valid("not-a-uuid")

    # ---- random_bytes ----
    buf = _vlink.Uuid.random_bytes(32)
    assert isinstance(buf, bytes) and len(buf) == 32
    assert _vlink.Uuid.random_bytes(0) == b""
    # variability
    assert _vlink.Uuid.random_bytes(32) != _vlink.Uuid.random_bytes(32)

    # ---- random_hex ----
    hex16 = _vlink.Uuid.random_hex(16)
    assert isinstance(hex16, str) and len(hex16) == 32
    assert all(c in "0123456789abcdef" for c in hex16)
    assert _vlink.Uuid.random_hex() == _vlink.Uuid.random_hex().__class__("") or True  # tautology guard
    assert len(_vlink.Uuid.random_hex()) == 32
    assert _vlink.Uuid.random_hex(0) == ""
    assert len(_vlink.Uuid.random_hex(5)) == 10
    assert _vlink.Uuid.random_hex(16) != _vlink.Uuid.random_hex(16)

    # ---- random_hex round-trips through from_string for 16-byte width ----
    hex_token = _vlink.Uuid.random_hex(16)
    parsed = _vlink.Uuid.from_string(hex_token)
    assert parsed is not None
    assert parsed.to_compact_string() == hex_token

    # ---- unique set of 100 randoms ----
    bucket = {_vlink.Uuid.generate_random() for _ in range(100)}
    assert len(bucket) == 100

    print("[PASS] Uuid")


def test_zerocopy_header():
    """Test zerocopy.Header field round-trip."""
    h = _vlink.ZeroCopyHeader()
    h.frame_id = "lidar_top"
    h.seq = 42
    h.reserved = 7
    h.time_meas = 1_000_000_000
    h.time_pub = 1_000_000_500

    assert h.frame_id == "lidar_top"
    assert h.seq == 42
    assert h.reserved == 7
    assert h.time_meas == 1_000_000_000
    assert h.time_pub == 1_000_000_500

    print("[PASS] ZeroCopyHeader")


def test_zerocopy_raw_data():
    """Test RawData create + serialise round-trip."""
    rd = _vlink.RawData()
    rd.header.frame_id = "raw_test"
    rd.header.seq = 1
    assert rd.create(1024)
    assert rd.size() == 1024
    assert rd.is_valid()
    assert rd.is_owner()

    wire = rd.to_bytes()
    assert wire.size() == rd.get_serialized_size()
    assert _vlink.RawData.check_valid(wire)

    rd2 = _vlink.RawData()
    assert rd2.from_bytes(wire)
    assert rd2.size() == 1024
    assert rd2.header.seq == 1
    assert rd2.header.frame_id == "raw_test"

    print("[PASS] RawData")


def test_zerocopy_camera_frame():
    """Test CameraFrame create + serialise round-trip."""
    cf = _vlink.CameraFrame()
    cf.set_width(640)
    cf.set_height(480)
    cf.set_channel(3)
    cf.set_freq(30)
    cf.set_format(_vlink.CameraFrame.Format.Nv12)
    cf.set_stream(_vlink.CameraFrame.Stream.I)
    assert cf.create(640 * 480 * 3 // 2)
    assert cf.is_valid()

    wire = cf.to_bytes()
    assert _vlink.CameraFrame.check_valid(wire)

    cf2 = _vlink.CameraFrame()
    assert cf2.from_bytes(wire)
    assert cf2.width() == 640
    assert cf2.height() == 480
    assert cf2.format() == _vlink.CameraFrame.Format.Nv12
    assert cf2.stream() == _vlink.CameraFrame.Stream.I
    assert _vlink.CameraFrame.Format.Float32C1 is not None
    assert _vlink.CameraFrame.format_from_encoding("32FC1") == _vlink.CameraFrame.Format.Float32C1
    assert _vlink.CameraFrame.format_from_encoding("bayer_rggb8") == _vlink.CameraFrame.Format.BayerRggb8
    assert _vlink.CameraFrame.encoding_from_format(_vlink.CameraFrame.Format.Webp) == "webp"

    print("[PASS] CameraFrame")


def test_zerocopy_point_cloud():
    """Test PointCloud create + push + serialise round-trip."""
    pc = _vlink.PointCloud()
    # 3 fields x,y,z each 4-byte float: size_num = 0x444 = 1092, type_num = 0xAAA = 2730
    size_num = (4 << 8) | (4 << 4) | 4
    type_num = (10 << 8) | (10 << 4) | 10
    assert pc.create(100, size_num, type_num, "x,y,z")
    assert pc.pack_size() == 12

    assert pc.push_value_v3f(1.0, 2.0, 3.0)
    assert pc.push_value_v3f(4.0, 5.0, 6.0)
    assert pc.size() == 2

    v = pc.get_value_v3f(0)
    assert abs(v.x - 1.0) < 1e-6 and abs(v.y - 2.0) < 1e-6 and abs(v.z - 3.0) < 1e-6

    wire = pc.to_bytes()
    assert _vlink.PointCloud.check_valid(wire)

    pc2 = _vlink.PointCloud()
    assert pc2.from_bytes(wire)
    assert pc2.size() == 2
    v2 = pc2.get_value_v3f(1)
    assert abs(v2.x - 4.0) < 1e-6

    print("[PASS] PointCloud")


def test_zerocopy_point_cloud_compress():
    """Test PointCloud compression: int16 quantize + vertical layout round-trip."""
    pc = _vlink.PointCloud()
    size_num = (4 << 8) | (4 << 4) | 4
    type_num = (10 << 8) | (10 << 4) | 10
    # extent 10 -> XYZ in [-10, 10] stored as int16 (pack 12 -> 6); vertical -> SoA on the wire.
    assert pc.create(1000, size_num, type_num, "x,y,z", 10, False)
    assert pc.get_extent() == 10
    assert pc.get_vertical() is False
    pc.set_vertical(True)
    assert pc.get_vertical() is True
    assert pc.pack_size() == 6

    assert pc.push_value_v3f(1.234, -5.678, 9.012)
    qx, qy, qz = struct.unpack("hhh", pc.data()[:6])
    assert qx == _vlink.quantize.encode_int16(10.0, 1.234)
    assert qy == _vlink.quantize.encode_int16(10.0, -5.678)
    assert qz == _vlink.quantize.encode_int16(10.0, 9.012)

    v = pc.get_value_v3f(0)
    assert abs(v.x - 1.234) < 1e-3 and abs(v.y + 5.678) < 1e-3 and abs(v.z - 9.012) < 1e-3

    wire = pc.to_bytes()
    assert _vlink.PointCloud.check_valid(wire)

    rx = _vlink.PointCloud()
    assert rx.from_bytes(wire)
    assert rx.get_extent() == 10
    assert rx.get_vertical() is True
    v2 = rx.get_value_v3f(0)
    assert abs(v2.x - 1.234) < 1e-3 and abs(v2.y + 5.678) < 1e-3 and abs(v2.z - 9.012) < 1e-3

    rx.set_vertical(False)
    assert rx.get_vertical() is False
    wire2 = rx.to_bytes()
    rx2 = _vlink.PointCloud()
    assert rx2.from_bytes(wire2)
    assert rx2.get_vertical() is False
    v3 = rx2.get_value_v3f(0)
    assert abs(v3.x - 1.234) < 1e-3 and abs(v3.y + 5.678) < 1e-3 and abs(v3.z - 9.012) < 1e-3

    print("[PASS] PointCloud compression")


def test_zerocopy_proxy_data():
    """Test ProxyData create + serialise round-trip."""
    pd = _vlink.ProxyData()
    pd.set_control_id(99)
    pd.set_seq(123)
    pd.set_timestamp(1_700_000_000)
    pd.set_schema(int(_vlink.SchemaType.Raw))
    raw = _vlink.Bytes.from_bytes(b"hello-payload")
    pd.create(raw, "intra://test/proxy", "demo.RawBytes", int(_vlink.SchemaType.Raw), "host01")

    assert pd.is_valid()
    assert pd.url() == "intra://test/proxy"
    assert pd.ser() == "demo.RawBytes"
    assert pd.hostname() == "host01"
    assert pd.control_id() == 99
    assert pd.seq() == 123

    wire = pd.to_bytes()
    assert _vlink.ProxyData.check_valid(wire)

    pd2 = _vlink.ProxyData()
    assert pd2.from_bytes(wire)
    assert pd2.url() == "intra://test/proxy"
    assert pd2.ser() == "demo.RawBytes"
    assert pd2.hostname() == "host01"
    assert pd2.control_id() == 99

    print("[PASS] ProxyData")


def test_zerocopy_occupancy_grid():
    """Test OccupancyGrid create + serialise round-trip."""
    og = _vlink.OccupancyGrid()
    og.set_width(40)
    og.set_height(40)
    og.set_resolution(0.05)
    og.set_cell_type(_vlink.OccupancyGrid.CellType.Int8)
    og.set_default_value(-1)
    og.set_map_id("lvl_1")
    assert og.create(40 * 40)
    assert og.cell_size() == 1
    assert og.is_valid()

    wire = og.to_bytes()
    assert _vlink.OccupancyGrid.check_valid(wire)

    og2 = _vlink.OccupancyGrid()
    assert og2.from_bytes(wire)
    assert og2.width() == 40
    assert og2.height() == 40
    assert og2.cell_type() == _vlink.OccupancyGrid.CellType.Int8
    assert og2.default_value() == -1
    assert og2.map_id() == "lvl_1"

    print("[PASS] OccupancyGrid")


def test_zerocopy_tensor():
    """Test Tensor shape + dtype + serialise round-trip."""
    t = _vlink.Tensor()
    t.set_name("image")
    t.set_layout("NCHW")
    t.set_dtype(_vlink.Tensor.DataType.Float32)
    t.set_shape([1, 3, 224, 224])
    expected_elements = 1 * 3 * 224 * 224
    assert t.rank() == 4
    assert t.num_elements() == expected_elements
    assert t.element_size() == 4
    assert t.create(expected_elements * 4)
    assert t.is_valid()

    wire = t.to_bytes()
    assert _vlink.Tensor.check_valid(wire)

    t2 = _vlink.Tensor()
    assert t2.from_bytes(wire)
    assert t2.rank() == 4
    assert t2.num_elements() == expected_elements
    assert t2.shape() == [1, 3, 224, 224]
    assert t2.dtype() == _vlink.Tensor.DataType.Float32
    assert t2.name() == "image"
    assert t2.layout() == "NCHW"

    many_dims = _vlink.Tensor()
    many_dims.set_shape([1] * 300)
    assert many_dims.rank() == _vlink.Tensor.kMaxRank
    assert many_dims.shape() == [1] * _vlink.Tensor.kMaxRank

    print("[PASS] Tensor")


def test_zerocopy_object_array():
    """Test ObjectArray push + objects(i) + serialise round-trip."""
    arr = _vlink.ObjectArray()
    assert arr.create(8)
    arr.set_source_id("fusion")

    obj = _vlink.ObjectArray.Object()
    obj.label = "car"
    obj.position = [1.0, 2.0, 3.0]
    obj.size = [4.5, 1.8, 1.6]
    obj.yaw = 0.1
    obj.velocity = [8.5, 0.0, 0.0]
    obj.class_id = 1
    obj.track_id = 42
    obj.motion_state = _vlink.ObjectArray.MotionState.Moving
    obj.source_type = _vlink.ObjectArray.SourceType.Fusion
    assert arr.push_value(obj)
    assert arr.count() == 1

    wire = arr.to_bytes()
    assert _vlink.ObjectArray.check_valid(wire)

    arr2 = _vlink.ObjectArray()
    assert arr2.from_bytes(wire)
    assert arr2.count() == 1
    assert arr2.source_id() == "fusion"
    got = arr2.objects(0)
    assert got is not None
    assert got.label == "car"
    assert got.class_id == 1
    assert got.track_id == 42
    assert got.motion_state == _vlink.ObjectArray.MotionState.Moving
    pos = got.position
    assert abs(pos[0] - 1.0) < 1e-6 and abs(pos[1] - 2.0) < 1e-6 and abs(pos[2] - 3.0) < 1e-6

    print("[PASS] ObjectArray")


def test_zerocopy_audio_frame():
    """Test AudioFrame create + serialise round-trip."""
    af = _vlink.AudioFrame()
    af.set_sample_rate(48000)
    af.set_num_channels(2)
    af.set_num_samples(960)
    af.set_bit_depth(16)
    af.set_format(_vlink.AudioFrame.Format.PcmS16)
    af.set_layout(_vlink.AudioFrame.Layout.Interleaved)
    af.set_codec("PCM")
    af.set_language("en")
    assert af.create(960 * 2 * 2)
    assert af.is_valid()

    wire = af.to_bytes()
    assert _vlink.AudioFrame.check_valid(wire)

    af2 = _vlink.AudioFrame()
    assert af2.from_bytes(wire)
    assert af2.sample_rate() == 48000
    assert af2.num_channels() == 2
    assert af2.format() == _vlink.AudioFrame.Format.PcmS16
    assert af2.layout() == _vlink.AudioFrame.Layout.Interleaved
    assert af2.codec() == "PCM"
    assert af2.language() == "en"

    print("[PASS] AudioFrame")



if __name__ == "__main__":
    _vlink.Logger.init("py_test")
    print(f"VLink Python Bindings Test - v{_vlink.VERSION}")
    print("=" * 50)

    test_bytes()
    test_uuid()
    test_pubsub()
    test_rpc()
    test_field()
    test_message_loop()
    test_thread_pool()
    test_utils()
    test_quantize()
    test_qos()
    test_zerocopy_header()
    test_zerocopy_raw_data()
    test_zerocopy_camera_frame()
    test_zerocopy_point_cloud()
    test_zerocopy_point_cloud_compress()
    test_zerocopy_proxy_data()
    test_zerocopy_occupancy_grid()
    test_zerocopy_tensor()
    test_zerocopy_object_array()
    test_zerocopy_audio_frame()

    print("=" * 50)
    print("ALL TESTS PASSED!")

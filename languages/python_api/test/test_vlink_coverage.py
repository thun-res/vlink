#!/usr/bin/env python3
"""Coverage test for VLink Python bindings.

Targets binding surface NOT exercised by test_vlink.py / test_vlink_full.py:
  * Node __repr__ output for plain and Security* variants
  * SpinLock (lock/try_lock/unlock + context manager)
  * CpuProfiler + CpuProfilerGuard
  * MemoryPool direct API (Config/TierStats/OversizedStats/clear/trim/global_instance)
  * MultiLoop construction
  * Logger level setters + file_handler + is_busy
  * Helpers leftover: to_int/to_long/format_milliseconds/format_file_size/format_rate_size/
                       split/split_any/get_hash_code
  * Quantize helper submodule visibility and int16 path
  * Utils leftover: get_app_dir/get_app_name/get_env/set_env/unset_env/yield_cpu/
                    try_release_sys_memory/set_thread_priority/set_thread_stick/
                    set_console_utf8_output/get_all_ipv6_address/get_interface_name_by_ipv4
  * Bytes leftover: from_string/init_memory_pool guard/__getitem__ positive/negative bounds/
                    convert_to_hex_str (static)/__bytes__/copy via deep_copy_self
  * ElapsedTimer extra ctors + restart + get_cpu_timestamp
  * DeadlineTimer set_deadline + has_expired
  * ThreadPool set_name/get_name/get_task_count/is_in_work_thread
  * Process get_max_buffer_size/set_max_buffer_size/get_pid+is_running
  * Bytes::Bytes(loan) is_loaned helpers / capacity / clear / __bool__
"""

import os
import subprocess
import sys
import threading
import time
import tempfile

os.environ["VLINK_DISCOVER_DISABLE"] = "1"

import vlink as _vlink  # type: ignore

assert getattr(_vlink, "__backend__", None) == "nanobind"


def test_node_repr_uses_python_class_name():
    """Verify __repr__ shows the correct class name (regression for Security* variants)."""
    pub = _vlink.Publisher("intra://test/repr_pub", auto_init=False)
    pub.set_discovery_enabled(False)
    pub.init()
    assert repr(pub).startswith("Publisher(")
    pub.deinit()

    sub = _vlink.Subscriber("intra://test/repr_sub", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.init()
    assert repr(sub).startswith("Subscriber(")
    sub.deinit()

    setter = _vlink.Setter("intra://test/repr_setter", auto_init=False)
    setter.set_discovery_enabled(False)
    setter.init()
    assert repr(setter).startswith("Setter(")
    setter.deinit()

    getter = _vlink.Getter("intra://test/repr_getter", auto_init=False)
    getter.set_discovery_enabled(False)
    getter.init()
    assert repr(getter).startswith("Getter(")
    getter.deinit()

    srv = _vlink.Server("intra://test/repr_srv", auto_init=False)
    srv.set_discovery_enabled(False)
    srv.init()
    assert repr(srv).startswith("Server(")
    srv.deinit()

    cli = _vlink.Client("intra://test/repr_cli", auto_init=False)
    cli.set_discovery_enabled(False)
    cli.init()
    assert repr(cli).startswith("Client(")
    cli.deinit()

    ffs = _vlink.FireForgetServer("intra://test/repr_ffs", auto_init=False)
    ffs.set_discovery_enabled(False)
    ffs.init()
    assert repr(ffs).startswith("FireForgetServer(")
    ffs.deinit()

    ffc = _vlink.FireForgetClient("intra://test/repr_ffc", auto_init=False)
    ffc.set_discovery_enabled(False)
    ffc.init()
    assert repr(ffc).startswith("FireForgetClient(")
    ffc.deinit()

    cfg = _vlink.SecurityConfig()
    cfg.key = "coverage-key"

    sec_pub = _vlink.SecurityPublisher("shm://test/repr_sec_pub", cfg, auto_init=False)
    assert repr(sec_pub).startswith("SecurityPublisher(")

    sec_sub = _vlink.SecuritySubscriber("shm://test/repr_sec_sub", cfg, auto_init=False)
    assert repr(sec_sub).startswith("SecuritySubscriber(")

    sec_srv = _vlink.SecurityServer("shm://test/repr_sec_srv", cfg, auto_init=False)
    assert repr(sec_srv).startswith("SecurityServer(")

    sec_cli = _vlink.SecurityClient("shm://test/repr_sec_cli", cfg, auto_init=False)
    assert repr(sec_cli).startswith("SecurityClient(")

    sec_ffs = _vlink.SecurityFireForgetServer("shm://test/repr_sec_ffs", cfg, auto_init=False)
    assert repr(sec_ffs).startswith("SecurityFireForgetServer(")

    sec_ffc = _vlink.SecurityFireForgetClient("shm://test/repr_sec_ffc", cfg, auto_init=False)
    assert repr(sec_ffc).startswith("SecurityFireForgetClient(")

    sec_set = _vlink.SecuritySetter("shm://test/repr_sec_set", cfg, auto_init=False)
    assert repr(sec_set).startswith("SecuritySetter(")

    sec_get = _vlink.SecurityGetter("shm://test/repr_sec_get", cfg, auto_init=False)
    assert repr(sec_get).startswith("SecurityGetter(")

    print("[PASS] Node __repr__ for plain and Security* variants")


def test_spin_lock():
    sl = _vlink.SpinLock()
    sl.lock()
    sl.unlock()

    assert sl.try_lock() is True
    sl.unlock()

    # Keep a possible GIL deadlock contained so a regression fails by timeout
    # instead of hanging the complete test process.
    contention_check = """
import threading
import time
import vlink

lock = vlink.SpinLock()
lock.lock()
acquired = threading.Event()

def wait_for_lock():
    lock.lock()
    acquired.set()
    lock.unlock()

waiter = threading.Thread(target=wait_for_lock)
waiter.start()
time.sleep(0.02)
lock.unlock()
waiter.join(timeout=1.0)
assert acquired.is_set()
"""
    subprocess.run(
        [sys.executable, "-c", contention_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=3.0,
    )

    fired = []
    with sl as guarded:
        assert guarded is sl
        fired.append(1)
    assert fired == [1]
    assert sl.try_lock() is True
    sl.unlock()

    print("[PASS] SpinLock lock/try_lock/unlock + context manager")


def test_cpu_profiler():
    profiler = _vlink.CpuProfiler()
    profiler.begin()
    for _ in range(1000):
        pass
    profiler.end()
    profiler.get()
    profiler.restart()

    assert isinstance(_vlink.CpuProfiler.is_global_enabled(), bool)

    with profiler as entered:
        assert entered is profiler
        time.sleep(0.001)

    guard = _vlink.CpuProfilerGuard(profiler)
    del guard
    print("[PASS] CpuProfiler + CpuProfilerGuard")


def test_memory_pool_direct():
    cfg = _vlink.MemoryPool.get_default_config()
    assert cfg.prealloc in (True, False)
    assert isinstance(cfg.tiers, list)

    pool = _vlink.MemoryPool()
    tier_count = pool.get_tier_count()
    assert tier_count >= 0

    stats_list = pool.get_stats()
    assert isinstance(stats_list, list)
    assert len(stats_list) == tier_count
    for stats in stats_list:
        assert stats.max_size >= 0
        assert stats.blocks_per_chunk >= 0
        assert stats.in_use_blocks >= 0
        assert stats.chunk_count >= 0
        assert stats.hit_count >= 0
        assert stats.deallocate_count >= 0
        assert stats.upstream_alloc_count >= 0
        assert stats.upstream_alloc_bytes >= 0

    oversized = pool.get_oversized_stats()
    assert oversized.alloc_count >= 0
    assert oversized.alloc_bytes >= 0
    assert oversized.dealloc_count >= 0

    pool.reset_stats()
    pool.trim()
    pool.clear()

    pool_lv = _vlink.MemoryPool(1, prealloc=False)
    assert pool_lv.get_tier_count() >= 0

    pool_cfg = _vlink.MemoryPool(cfg)
    assert pool_cfg.get_tier_count() >= 0

    global_pool = _vlink.MemoryPool.global_instance()
    assert global_pool is not None
    assert global_pool.get_tier_count() >= 0

    assert _vlink.MemoryPool.kBlockAlignment > 0
    print("[PASS] MemoryPool direct (config/stats/global/clear/trim)")


def test_memory_pool_tier_config():
    tier = _vlink.MemoryPool.Tier(64, 16)
    assert tier.max_size == 64
    assert tier.blocks_per_chunk == 16

    cfg = _vlink.MemoryPool.Config()
    assert cfg.batch_size == 16
    cfg.tiers = [tier]
    cfg.prealloc = False
    cfg.batch_size = 1
    pool = _vlink.MemoryPool(cfg)
    assert pool.get_tier_count() == 1
    s = pool.get_stats()
    assert len(s) == 1
    assert s[0].max_size == 64
    assert s[0].blocks_per_chunk == 16
    print("[PASS] MemoryPool Tier/Config build")


def test_multi_loop():
    loop = _vlink.MultiLoop(2)
    assert isinstance(loop, _vlink.MessageLoop)
    loop.set_name("py_multi")
    assert loop.get_name() == "py_multi"

    done = threading.Event()
    counter = [0]
    lock = threading.Lock()

    def task():
        with lock:
            counter[0] += 1

        if counter[0] >= 4:
            done.set()

    loop.async_run()
    for _ in range(4):
        loop.post_task(task)
    done.wait(2.0)
    assert counter[0] >= 4

    loop.quit()
    loop.wait_for_quit(2000)

    loop2 = _vlink.MultiLoop(2, _vlink.MessageLoopType.Lockfree)
    assert loop2.get_type() == _vlink.MessageLoopType.Lockfree
    print("[PASS] MultiLoop")


def test_logger_levels_and_file_handler():
    orig_console = _vlink.Logger.get_console_level()
    _vlink.Logger.set_console_level(_vlink.LogLevel.Debug)
    assert _vlink.Logger.get_console_level() == _vlink.LogLevel.Debug
    _vlink.Logger.set_console_level(orig_console)

    orig_file = _vlink.Logger.get_file_level()
    _vlink.Logger.set_file_level(_vlink.LogLevel.Warn)
    assert _vlink.Logger.get_file_level() == _vlink.LogLevel.Warn
    _vlink.Logger.set_file_level(orig_file)

    assert isinstance(_vlink.Logger.is_busy(), bool)

    captured = []

    _vlink.Logger.register_file_handler(lambda lv, msg: captured.append((lv, msg)))
    _vlink.log_warn("file-handler-test-message")
    _vlink.Logger.flush()
    for _ in range(20):
        if captured:
            break

        time.sleep(0.01)
    # Restore default null-ish handler
    _vlink.Logger.register_file_handler(lambda lv, msg: None)

    inst = _vlink.Logger.get()
    assert inst is not None
    print("[PASS] Logger levels + file handler + is_busy + get")


def test_helpers_leftover():
    assert _vlink.helpers.to_int("42") == 42
    assert _vlink.helpers.to_int("not-a-number", 99) == 99

    assert _vlink.helpers.to_long("12345678901") == 12345678901
    assert _vlink.helpers.to_long("nope", 7) == 7

    assert isinstance(_vlink.helpers.format_milliseconds(125000, show_millis=True), str)
    assert isinstance(_vlink.helpers.format_milliseconds(125000, show_millis=False), str)

    assert isinstance(_vlink.helpers.format_file_size(1536), str)
    assert isinstance(_vlink.helpers.format_rate_size(1024 * 1024), str)

    parts = _vlink.helpers.split("a,b,c,d", ",")
    assert parts == ["a", "b", "c", "d"]
    assert _vlink.helpers.split("a,b,c") == ["a", "b", "c"]

    filter_parts = _vlink.helpers.split_any(" 192.168.1.1,10.0.0.2  172.16.0.1 ")
    assert filter_parts == ["192.168.1.1", "10.0.0.2", "172.16.0.1"]
    assert _vlink.helpers.split_any("a;b,c") == ["a;b", "c"]
    assert _vlink.helpers.split_any("a|b/c", "|/") == ["a", "b", "c"]
    assert _vlink.helpers.split_any(" a,b c ", "") == ["a,b c"]

    filter_view_parts = _vlink.helpers.split_any_view("192.168.1.1|10.0.0.2,172.16.0.1")
    assert filter_view_parts == ["192.168.1.1|10.0.0.2", "172.16.0.1"]
    assert _vlink.helpers.split_any_view("a;b,c") == ["a;b", "c"]
    assert _vlink.helpers.split_any_view(" \t\r\n ") == []
    assert _vlink.helpers.split_any_view("a|b/c", "|/") == ["a", "b", "c"]
    assert _vlink.helpers.split_any_view(" a,b c ", "") == ["a,b c"]

    h = _vlink.helpers.get_hash_code("vlink")
    assert isinstance(h, int) and h > 0

    assert _vlink.helpers.has_startwith("prefix-x", "prefix")
    assert _vlink.helpers.has_endwith("x-suffix", "suffix")
    assert _vlink.helpers.trim_string_view("  view  ") == "view"
    print("[PASS] Helpers leftover (to_int/to_long/format_*/split/split_any/get_hash_code)")


def test_quantize_submodule():
    assert hasattr(_vlink, "quantize")
    assert _vlink.quantize.encode_int16(-10.0, 10.0, 0.0) == 0
    assert _vlink.quantize.encode_int16(-10.0, 10.0, 10.0) == 32767
    assert abs(_vlink.quantize.decode_int16(-10.0, 10.0, 32767) - 10.0) < 1e-6
    assert _vlink.quantize.encode_int16(10.0, 10.0) == 32767
    assert abs(_vlink.quantize.decode_int16(10.0, 32767) - 10.0) < 1e-6
    print("[PASS] Quantize submodule")


def test_utils_leftover():
    assert isinstance(_vlink.utils.get_app_dir(), str)
    assert isinstance(_vlink.utils.get_app_name(), str)
    assert isinstance(_vlink.utils.get_host_name(), str)
    assert isinstance(_vlink.utils.get_pid_str(), str)
    assert isinstance(_vlink.utils.get_tmp_dir(), str)

    _vlink.utils.set_env("VLINK_PY_TEST_VAR", "hello", True)
    assert _vlink.utils.get_env("VLINK_PY_TEST_VAR", "fallback") == "hello"
    _vlink.utils.unset_env("VLINK_PY_TEST_VAR")
    assert _vlink.utils.get_env("VLINK_PY_TEST_VAR", "fallback") == "fallback"

    _vlink.utils.yield_cpu()
    _vlink.utils.try_release_sys_memory()
    _vlink.utils.set_console_utf8_output()

    # Best-effort: these may return False without permissions or on unsupported platforms.
    assert isinstance(_vlink.utils.set_thread_priority(0), bool)
    assert isinstance(_vlink.utils.set_thread_stick(0x1), bool)

    addrs6 = _vlink.utils.get_all_ipv6_address()
    assert isinstance(addrs6, list)

    addrs4 = _vlink.utils.get_all_ipv4_address()
    assert isinstance(addrs4, list)
    if addrs4:
        name = _vlink.utils.get_interface_name_by_ipv4(addrs4[0])
        assert isinstance(name, str)

    print("[PASS] Utils leftover (env/yield/threads/network/console)")


def test_bytes_leftover():
    s = _vlink.Bytes.from_string("hello")
    assert s.to_string() == "hello"
    assert s.size() == 5

    raw = _vlink.Bytes.from_bytes(b"\xde\xad\xbe\xef")
    assert _vlink.Bytes.convert_to_hex_str(raw) in ("DE AD BE EF", "deadbeef", "de ad be ef", "DEADBEEF")

    assert bytes(raw) == b"\xde\xad\xbe\xef"
    assert raw[0] == 0xDE
    assert raw[-1] == 0xEF
    assert raw[-len(raw)] == 0xDE

    try:
        _ = raw[100]
        assert False, "expected IndexError"
    except IndexError:
        pass

    try:
        _ = raw[-len(raw) - 1]
        assert False, "expected IndexError"
    except IndexError:
        pass

    empty = _vlink.Bytes()
    assert not bool(empty)
    assert empty.size() == 0
    empty.clear()
    assert empty.size() == 0

    big = _vlink.Bytes.create(64)
    cap = big.capacity()
    assert cap >= 64
    big.shrink_to(8)
    assert big.size() == 8

    # deep_copy_self on an existing owner is a no-op but should not throw
    owner = _vlink.Bytes.from_bytes(b"abc")
    assert owner.is_owner()
    owner.deep_copy_self()
    assert owner.to_bytes() == b"abc"

    # static init/release pool calls must be reentrant-safe
    _vlink.Bytes.init_memory_pool()
    _vlink.Bytes.init_memory_pool()
    _vlink.Bytes.release_memory_pool()

    print("[PASS] Bytes leftover (from_string/convert_to_hex_str/__bytes__/bounds/clear/static-pool)")


def test_elapsed_timer_extra_ctors():
    default = _vlink.ElapsedTimer()
    default.start()
    default.stop()
    default.restart()
    assert default.is_active()
    default.stop()

    method_only = _vlink.ElapsedTimer(_vlink.TimerMethod.CpuActiveTime)
    assert method_only.get_method() == _vlink.TimerMethod.CpuActiveTime

    accuracy_only = _vlink.ElapsedTimer(_vlink.TimerAccuracy.Nano)
    assert accuracy_only.get_accuracy() == _vlink.TimerAccuracy.Nano

    cpu_ts = _vlink.ElapsedTimer.get_cpu_timestamp(_vlink.TimerAccuracy.Milli, True)
    assert cpu_ts >= 0
    print("[PASS] ElapsedTimer extra ctors / get_cpu_timestamp")


def test_deadline_timer_extra():
    dt = _vlink.DeadlineTimer()
    assert not dt.is_valid()
    dt.set_deadline(50)
    assert dt.is_valid()
    assert isinstance(dt.has_expired(), bool)
    time.sleep(0.1)
    assert dt.has_expired()
    print("[PASS] DeadlineTimer set_deadline / has_expired")


def test_thread_pool_management():
    pool = _vlink.ThreadPool(2)
    pool.set_name("py_audit_pool")
    assert pool.get_name() == "py_audit_pool"
    assert isinstance(pool.is_in_work_thread(), bool)
    assert pool.get_task_count() >= 0
    pool.shutdown()
    print("[PASS] ThreadPool management (set_name/get_name/get_task_count/is_in_work_thread)")


def test_process_buffer_and_pid():
    p = _vlink.Process()
    p.set_max_buffer_size(64 * 1024)
    assert p.get_max_buffer_size() == 64 * 1024
    assert p.get_process_id() == -1
    assert not p.is_running()
    assert p.get_state() == _vlink.Process.State.NotRunning
    assert p.get_error() == _vlink.Process.Error.NoError
    assert isinstance(p.get_exit_code(), int)

    state_changes = []
    p.register_state_changed_callback(lambda s: state_changes.append(s))
    p.set_process_mode(_vlink.Process.Mode.Merged)
    p.start("/bin/true")
    p.wait_for_finished(5000)
    # After wait_for_finished, get_process_id may be reset to -1; only assert it's an int.
    assert isinstance(p.get_process_id(), int)
    time.sleep(0.1)
    # state changes captured at some point
    assert state_changes
    print("[PASS] Process buffer/pid/state-changed callback")


def test_uuid_engine_overload_smoke():
    # Verify the no-engine static overload selection succeeded (already used in test_vlink.py
    # but we additionally check that consecutive generations are distinct under threads).
    seen = set()
    lock = threading.Lock()

    def worker():
        for _ in range(50):
            u = _vlink.Uuid.generate_random()
            with lock:
                seen.add(u)

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()

    for t in threads:
        t.join()
    assert len(seen) >= 199  # tolerate the rare hash collision
    print("[PASS] Uuid thread-safe generate_random")


def test_schema_data_default_bool():
    s = _vlink.SchemaData()
    assert not s
    s.encoding = "protobuf"
    assert s

    raw = _vlink.RawData()
    assert raw.create(4096)
    assert raw.fill_data(b"A" * 4096)
    s.data = raw.to_bytes()
    wire = s.data
    parsed = _vlink.RawData()
    assert parsed.from_bytes(wire)

    # Replacing a Bytes-valued member used to bypass Bytes.resize/clear guards
    # and leave parsed pointing at freed storage.
    try:
        s.data = _vlink.Bytes.from_bytes(b"replacement")
        assert False, "SchemaData.data must not replace pinned storage"
    except BufferError:
        pass
    assert parsed.data() == b"A" * 4096

    # Assigning the exact same member is a true no-op and remains valid.
    s.data = wire
    del parsed
    s.data = _vlink.Bytes.from_bytes(b"replacement")
    assert s.data.to_bytes() == b"replacement"
    print("[PASS] SchemaData __bool__ / pinned data replacement")


def test_sample_lost_info():
    info = _vlink.SampleLostInfo()
    info.total = 100
    info.lost = 5
    assert info.total == 100
    assert info.lost == 5
    assert "100" in repr(info)
    print("[PASS] SampleLostInfo")


def test_status_helpers():
    assert _vlink.Status.is_for_writer(_vlink.StatusType.PublicationMatched)
    assert _vlink.Status.is_for_reader(_vlink.StatusType.SubscriptionMatched)
    assert not _vlink.Status.is_for_writer(_vlink.StatusType.SubscriptionMatched)
    assert not _vlink.Status.is_for_reader(_vlink.StatusType.PublicationMatched)
    print("[PASS] Status writer/reader classification")


def test_qos_profile_constants():
    for name in ("Event", "Method", "Field", "Sensor", "Parameter", "Service",
                 "Clock", "Static", "Light", "Poor", "Better", "Best", "Large",
                 "Alarm", "Command", "Log"):
        assert hasattr(_vlink.QosProfile, name)
        qos = getattr(_vlink.QosProfile, name)
        assert isinstance(qos, _vlink.Qos)
        assert qos.deadline.period == -1
        assert qos.lifespan.duration == -1

    # The runtime lookup map must expose every preset by its string name.
    qos_map = _vlink.QosProfile.get_available_qos_map()
    for key in ("event", "method", "field", "sensor", "parameter", "service",
                "clock", "static", "light", "poor", "better", "best", "large",
                "alarm", "command", "log"):
        assert key in qos_map, f"missing preset: {key}"
    assert len(qos_map) >= 16

    # Spot-check a couple of fields on the newly added presets.
    assert _vlink.QosProfile.Alarm.additions.is_express is True
    assert _vlink.QosProfile.Log.history.depth == 100
    print("[PASS] QosProfile constants")


def test_version_helpers():
    a = _vlink.Version(1, 0, 0)
    b = _vlink.Version(1, 0, 0)
    c = _vlink.Version(2, 0, 0)
    assert a == b
    assert a != c
    assert a < c
    assert c > a
    assert a.is_valid()
    s = a.to_string()
    assert "1" in s
    print("[PASS] Version comparison + to_string")


def test_publish_force_and_loan_query():
    pub = _vlink.Publisher("intra://test/force_publish", auto_init=False)
    pub.set_discovery_enabled(False)
    pub.init()
    # No subscribers -> publish without force returns False; with force returns True.
    assert pub.publish(b"x") is False
    assert pub.publish(b"x", force=True) is True
    # Loan API is queryable even on transports that don't support it.
    assert isinstance(pub.is_support_loan(), bool)
    pub.deinit()
    print("[PASS] Publisher.publish force + is_support_loan")


def test_subscriber_latency_and_lost_flags():
    """intra:// doesn't actually track latency, but the binding surface must be callable."""
    sub = _vlink.Subscriber("intra://test/latency_flags", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.init()
    sub.set_latency_and_lost_enabled(True)
    assert isinstance(sub.is_latency_and_lost_enabled(), bool)
    assert isinstance(sub.get_latency(), int)
    lost = sub.get_lost()
    assert isinstance(lost.total, int)
    assert isinstance(lost.lost, int)
    sub.set_latency_and_lost_enabled(False)
    sub.deinit()
    print("[PASS] Subscriber latency-and-lost flags (binding callable)")


def test_getter_latency_flags():
    g = _vlink.Getter("intra://test/getter_latency", auto_init=False)
    g.set_discovery_enabled(False)
    g.init()
    g.set_latency_and_lost_enabled(True)
    assert isinstance(g.is_latency_and_lost_enabled(), bool)
    assert isinstance(g.get_latency(), int)
    info = g.get_lost()
    assert isinstance(info.total, int)
    g.set_latency_and_lost_enabled(False)
    g.deinit()
    print("[PASS] Getter latency-and-lost flags (binding callable)")


def test_message_loop_post_task_failure_after_quit():
    loop = _vlink.MessageLoop()
    loop.async_run()
    loop.quit()
    loop.wait_for_quit(1000)
    # post_task on a quit loop should return False
    assert loop.post_task(lambda: None) is False
    print("[PASS] MessageLoop post_task returns False after quit")


def test_bytes_buffer_protocol_roundtrip():
    b = _vlink.Bytes.from_bytes(b"abc123")
    mv = memoryview(b)
    assert bytes(mv) == b"abc123"
    assert len(mv) == 6

    assert b.resize(b.size())
    assert b.reserve(b.capacity())
    assert b.shrink_to(b.size())
    assert b.deep_copy_self() is b

    for mutate in (
        lambda: b.clear(),
        lambda: b.resize(1024),
        lambda: b.reserve(1024),
        lambda: b.shrink_to(1),
    ):
        try:
            mutate()
            assert False, "buffer-invalidating mutation must reject an active memoryview"
        except BufferError:
            pass

    del mv
    assert b.resize(1024)
    print("[PASS] Bytes buffer-protocol round-trip")


def test_url_remap_reload_and_errors():
    remap = _vlink.UrlRemap()
    tmp = tempfile.mkdtemp()
    p1 = os.path.join(tmp, "r1.json")
    p2 = os.path.join(tmp, "r2.json")
    with open(p1, "w") as f:
        f.write('{"a://t1": "b://target1"}')

    with open(p2, "w") as f:
        f.write('{"x://t2": "y://target2"}')
    assert remap.load(p1)
    assert remap.convert("a://t1") == "b://target1"
    assert remap.reload(p2)
    assert remap.convert("x://t2") == "y://target2"

    bad = os.path.join(tmp, "missing.json")
    err_remap = _vlink.UrlRemap()
    assert not err_remap.load(bad)
    assert isinstance(err_remap.get_error_string(), str)
    print("[PASS] UrlRemap reload + load-error path")


if __name__ == "__main__":
    _vlink.Logger.init("py_coverage")
    print(f"VLink Coverage Test - v{_vlink.VERSION}")
    print("=" * 60)

    test_node_repr_uses_python_class_name()
    test_spin_lock()
    test_cpu_profiler()
    test_memory_pool_direct()
    test_memory_pool_tier_config()
    test_multi_loop()
    test_logger_levels_and_file_handler()
    test_helpers_leftover()
    test_quantize_submodule()
    test_utils_leftover()
    test_bytes_leftover()
    test_elapsed_timer_extra_ctors()
    test_deadline_timer_extra()
    test_thread_pool_management()
    test_process_buffer_and_pid()
    test_uuid_engine_overload_smoke()
    test_schema_data_default_bool()
    test_sample_lost_info()
    test_status_helpers()
    test_qos_profile_constants()
    test_version_helpers()
    test_publish_force_and_loan_query()
    test_subscriber_latency_and_lost_flags()
    test_getter_latency_flags()
    test_message_loop_post_task_failure_after_quit()
    test_bytes_buffer_protocol_roundtrip()
    test_url_remap_reload_and_errors()

    print("=" * 60)
    print("ALL COVERAGE TESTS PASSED!")

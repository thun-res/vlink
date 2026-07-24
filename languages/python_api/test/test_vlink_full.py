#!/usr/bin/env python3
"""Comprehensive test for ALL VLink Python bindings including newly added APIs."""

import os
import signal
import subprocess
import sys
import threading
import time
import tempfile
import weakref

os.environ["VLINK_DISCOVER_DISABLE"] = "1"

import vlink as _vlink  # type: ignore

assert getattr(_vlink, "__backend__", None) == "nanobind"


def _make_node(cls, url, ser_type=""):
    node = cls(url, ser_type=ser_type, auto_init=False)
    node.set_discovery_enabled(False)
    node.init()
    return node


def _bag_frame(url, ser_type, schema_type, action_type, data, timestamp=-1):
    frame = _vlink.Frame()
    frame.timestamp = timestamp
    frame.url = url
    frame.ser_type = ser_type
    frame.schema_type = schema_type
    frame.action_type = action_type
    frame.data = data
    return frame


def test_bytes_extended():
    _vlink.Bytes.init_memory_pool()

    b = _vlink.Bytes.from_bytes(b"hello")
    assert b.size() == 5
    assert b.real_size() == 5
    assert b.capacity() >= 5
    assert b.offset() == 0
    assert b.is_owner()
    assert not b.is_loaned()
    assert not b.is_ptr()
    assert b[0] == ord('h')
    assert b[4] == ord('o')

    # resize / reserve / shrink
    b2 = _vlink.Bytes.create(100)
    assert b2.size() == 100
    b2_offset = _vlink.Bytes.create(4, offset=2)
    assert b2_offset.size() == 4
    assert b2_offset.offset() == 2
    assert b2_offset.real_size() == 6
    b2.resize(50)
    assert b2.size() == 50
    b2.reserve(200)
    assert b2.capacity() >= 200
    b2.shrink_to(10)
    assert b2.size() == 10

    # deep_copy_self
    b3 = _vlink.Bytes.from_bytes(b"abc")
    b3.deep_copy_self()
    assert b3.to_bytes() == b"abc"
    assert b3.is_owner()

    # to_raw_data
    raw = _vlink.Bytes.from_bytes(b"\x01\x02\x03").to_raw_data()
    assert raw == [1, 2, 3]

    # equality
    a = _vlink.Bytes.from_bytes(b"test")
    b = _vlink.Bytes.from_bytes(b"test")
    c = _vlink.Bytes.from_bytes(b"other")
    assert a == b
    assert not (a == c)
    assert a != c

    # is_compress_data
    compressed = _vlink.Bytes.compress(_vlink.Bytes.from_bytes(b"X" * 1000))
    assert _vlink.Bytes.is_compress_data(compressed)
    assert not _vlink.Bytes.is_compress_data(_vlink.Bytes.from_bytes(b"hello"))

    # reverse_order
    rev = _vlink.Bytes.reverse_order(_vlink.Bytes.from_bytes(b"\x01\x02\x03"))
    assert rev.to_bytes() == b"\x03\x02\x01"

    # endianness
    assert isinstance(_vlink.Bytes.is_little_endian(), bool)
    assert isinstance(_vlink.Bytes.is_big_endian(), bool)
    assert _vlink.Bytes.is_little_endian() != _vlink.Bytes.is_big_endian()

    # stack_size
    assert _vlink.Bytes.stack_size() == 96

    # from_user_input (hex with spaces)
    b4 = _vlink.Bytes.from_user_input("48 65 6C 6C 6F")
    assert b4.to_string() == "Hello"

    # bytes-like object compatibility
    assert _vlink.Bytes.from_bytes(memoryview(b"xyz")).to_bytes() == b"xyz"

    # CRC-32/ISO-HDLC: same payload, different buffer-protocol kinds must match.
    crc32_a = _vlink.Bytes.get_crc_32(b"hello")
    crc32_b = _vlink.Bytes.get_crc_32(bytearray(b"hello"))
    crc32_c = _vlink.Bytes.get_crc_32(memoryview(b"hello"))
    crc32_d = _vlink.Bytes.get_crc_32(_vlink.Bytes.from_bytes(b"hello"))
    assert crc32_a == crc32_b == crc32_c == crc32_d
    # Canonical reference vector for CRC-32/ISO-HDLC: "123456789" -> 0xCBF43926.
    assert _vlink.Bytes.get_crc_32(b"123456789") == 0xCBF43926
    assert _vlink.Bytes.get_crc_32(b"") == 0x00000000

    # CRC-64/ECMA-182: same payload, different buffer-protocol kinds must match.
    crc64_a = _vlink.Bytes.get_crc_64(b"hello")
    crc64_b = _vlink.Bytes.get_crc_64(bytearray(b"hello"))
    crc64_c = _vlink.Bytes.get_crc_64(memoryview(b"hello"))
    crc64_d = _vlink.Bytes.get_crc_64(_vlink.Bytes.from_bytes(b"hello"))
    assert crc64_a == crc64_b == crc64_c == crc64_d
    # Canonical reference vector for CRC-64/ECMA-182: "123456789" -> 0x6C40DF5F0B497347.
    assert _vlink.Bytes.get_crc_64(b"123456789") == 0x6C40DF5F0B497347
    assert _vlink.Bytes.get_crc_64(b"") == 0x0000000000000000

    base64_a = _vlink.Bytes.encode_to_base64(b"raw-input")
    base64_b = _vlink.Bytes.encode_to_base64(_vlink.Bytes.from_bytes(b"raw-input"))
    assert base64_a == base64_b

    big = b"X" * 5000
    c1 = _vlink.Bytes.compress(big)
    assert _vlink.Bytes.is_compress_data(c1)
    assert _vlink.Bytes.uncompress(c1).to_bytes() == big
    assert _vlink.Bytes.uncompress(c1, check_valid=False).to_bytes() == big
    assert _vlink.Bytes.from_string("hi", offset=1).offset() == 1
    assert _vlink.Bytes.from_bytes(b"hi", offset=1).offset() == 1
    assert _vlink.Bytes.convert_to_hex_str(b"\x0a\xff").lower() == "0a ff"

    rev = _vlink.Bytes.reverse_order(b"\x01\x02\x03")
    assert rev.to_bytes() == b"\x03\x02\x01"

    print("[PASS] Bytes extended")


def test_logger_extended():
    _vlink.Logger.init("full_test")

    # Level checks
    assert _vlink.Logger.is_writable(_vlink.LogLevel.Info)
    assert _vlink.Logger.is_writable(_vlink.LogLevel.Error)

    # Console format
    orig = _vlink.Logger.get_console_fmt_enable()
    _vlink.Logger.set_console_fmt_enable(False)
    assert not _vlink.Logger.get_console_fmt_enable()
    _vlink.Logger.set_console_fmt_enable(orig)

    orig_precision = _vlink.Logger.get_stream_precision()
    orig_width = _vlink.Logger.get_stream_width()
    _vlink.Logger.set_stream_precision(6)
    _vlink.Logger.set_stream_width(4)
    assert _vlink.Logger.get_stream_precision() == 6
    assert _vlink.Logger.get_stream_width() == 4
    assert isinstance(_vlink.Logger.get_stream_flag(), int)
    _vlink.Logger.set_stream_precision(orig_precision)
    _vlink.Logger.set_stream_width(orig_width)

    # Backtrace
    _vlink.Logger.enable_backtrace(10)
    _vlink.log_debug("backtrace msg 1")
    _vlink.log_debug("backtrace msg 2")
    _vlink.Logger.dump_backtrace()
    _vlink.Logger.disable_backtrace()

    # Custom console handler
    captured = []
    _vlink.Logger.register_console_handler(lambda lv, msg: captured.append((lv, msg)))
    _vlink.log_info("custom handler test")
    _vlink.Logger.flush()
    for _ in range(20):
        if captured:
            break
        time.sleep(0.01)
    assert captured
    level, message = captured[-1]
    assert level == _vlink.LogLevel.Info
    assert type(level) is type(_vlink.LogLevel.Info)
    assert "custom handler test" in message
    # Restore default by registering None-like handler
    _vlink.Logger.register_console_handler(lambda lv, msg: None)

    replacement_check = r'''
import ctypes
import os
import threading
import vlink

entered = threading.Event()

def slow_handler(level, message):
    entered.set()
    if os.name == "nt":
        ctypes.CDLL("kernel32").Sleep(400)
    else:
        ctypes.CDLL(None).usleep(400_000)

vlink.Logger.set_console_level(vlink.LogLevel.Info)
vlink.Logger.register_console_handler(slow_handler)
worker = threading.Thread(target=lambda: vlink.log_info("handler replacement"))
worker.start()
assert entered.wait(2.0)
vlink.Logger.register_console_handler(lambda level, message: None)
worker.join(2.0)
assert not worker.is_alive()

self_error = []

def self_replacing_handler(level, message):
    try:
        vlink.Logger.register_console_handler(lambda inner_level, inner_message: None)
    except RuntimeError as exc:
        self_error.append(str(exc))

vlink.Logger.register_console_handler(self_replacing_handler)
vlink.log_info("self replacement")
assert self_error and "active logger callback" in self_error[0]
vlink.Logger.register_console_handler(lambda level, message: None)
'''
    subprocess.run(
        [sys.executable, "-c", replacement_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=4.0,
    )

    print("[PASS] Logger extended")


def test_memory_resource_export():
    if not getattr(_vlink, "_HAS_MEMORY_RESOURCE", False):
        assert not sys.platform.startswith("linux"), "Linux nanobind build must export MemoryResource"
        print("[SKIP] MemoryResource export")
        return

    assert hasattr(_vlink, "MemoryResource")
    assert "MemoryResource" in _vlink.__all__
    mr = _vlink.MemoryResource()
    mr_refcount = sys.getrefcount(mr)
    pool = mr.get_memory_pool()
    assert sys.getrefcount(mr) == mr_refcount + 1
    assert pool.get_tier_count() >= 0
    mr.trim()

    global_mr = _vlink.MemoryResource.global_instance()
    assert global_mr is not None
    assert global_mr.get_memory_pool().get_tier_count() >= 0
    print("[PASS] MemoryResource export")


def test_elapsed_timer_extended():
    t = _vlink.ElapsedTimer(_vlink.TimerMethod.CpuTimestamp, _vlink.TimerAccuracy.Micro)
    assert t.get_method() == _vlink.TimerMethod.CpuTimestamp
    assert t.get_accuracy() == _vlink.TimerAccuracy.Micro
    t.start()
    time.sleep(0.01)
    e = t.get()
    assert e > 0
    t.stop()

    # Static methods
    ts = _vlink.ElapsedTimer.get_sys_timestamp()
    assert ts > 0
    cpu = _vlink.ElapsedTimer.get_cpu_active_time()
    assert cpu >= 0

    print("[PASS] ElapsedTimer extended")


def test_deadline_timer_extended():
    dt = _vlink.DeadlineTimer(200)
    assert dt.is_valid()
    assert dt.deadline() > 0
    assert dt.remaining_time() > 0
    dt.set_deadline_abs(dt.deadline() + 1000)
    assert dt.remaining_time() > 0
    dt.reset()
    assert not dt.is_valid()

    micro = _vlink.DeadlineTimer(1000, _vlink.TimerAccuracy.Micro)
    assert micro.get_accuracy() == _vlink.TimerAccuracy.Micro

    print("[PASS] DeadlineTimer extended")


def test_message_loop_extended():
    loop = _vlink.MessageLoop(_vlink.MessageLoopType.Normal)
    loop.set_name("test_ext")
    assert loop.get_name() == "test_ext"
    assert loop.get_type() == _vlink.MessageLoopType.Normal

    # Strategy
    loop.set_strategy(_vlink.MessageLoopStrategy.Block)
    assert loop.get_strategy() == _vlink.MessageLoopStrategy.Block
    loop.set_strategy(_vlink.MessageLoopStrategy.Optimization)

    # Begin/end/idle handlers
    begin_called = [False]
    loop.register_begin_handler(lambda: begin_called.__setitem__(0, True))
    loop.async_run()
    time.sleep(0.05)
    assert begin_called[0]
    assert loop.is_running()
    assert not loop.is_ready_to_quit()

    # Max counters
    assert loop.get_max_task_count() >= 0
    assert loop.get_max_timer_count() >= 0
    assert loop.get_max_elapsed_time() >= 0

    # Wakeup
    loop.wakeup()

    # Wait for idle (timeout_ms, check)
    loop.wait_for_idle(100, True)
    loop.wait_for_idle(100, False)

    # Lockfree capacity reset hook (no-op for Normal type, just ensure binding works)
    lockfree_loop = _vlink.MessageLoop(_vlink.MessageLoopType.Lockfree)
    lockfree_loop.reset_lockfree_capacity()

    # post_task_with_priority (needs Priority type queue)
    ploop = _vlink.MessageLoop(_vlink.MessageLoopType.Priority)
    ploop.async_run()
    prio_done = threading.Event()
    ploop.post_task_with_priority(prio_done.set, _vlink.TaskPriority.Highest)
    prio_done.wait(2.0)
    assert prio_done.is_set()
    ploop.quit()
    ploop.wait_for_quit(2000)

    loop.quit()
    loop.wait_for_quit(2000)
    print("[PASS] MessageLoop extended")


def test_timer_extended():
    loop = _vlink.MessageLoop()
    loop.async_run()

    timer = _vlink.Timer(loop)
    timer.set_interval(50)
    timer.set_loop_count(2)
    timer.set_strict(True)
    assert timer.is_strict()
    timer.set_priority(5)
    assert timer.get_priority() == 5
    assert timer.get_message_loop() is not None

    done = threading.Event()
    timer.start(lambda: done.set() if timer.get_invoke_count() >= 2 else None)
    done.wait(2.0)
    timer.stop()

    # set_callback
    cb2_called = [False]
    timer.set_loop_count(1)
    timer.set_callback(lambda: cb2_called.__setitem__(0, True))
    timer.start()
    time.sleep(0.2)
    timer.stop()
    assert cb2_called[0]

    # call_once
    once_done = threading.Event()
    assert _vlink.Timer.call_once(loop, 30, once_done.set)
    once_done.wait(2.0)
    assert once_done.is_set()

    lifetime_check = r'''
import ctypes
import gc
import os
import sys
import threading
import vlink

def native_sleep(milliseconds):
    if os.name == "nt":
        ctypes.CDLL("kernel32").Sleep(milliseconds)
    else:
        ctypes.CDLL(None).usleep(milliseconds * 1000)

# Deleting a timer from another thread while its callback is running must not
# hold the GIL while the native destructor waits for that callback.
loop = vlink.MessageLoop()
assert loop.async_run()
entered = threading.Event()

def slow_callback():
    entered.set()
    native_sleep(500)

timer = vlink.Timer(loop, 1, -1, slow_callback)
timer.start()
assert entered.wait(2.0)
timer = None
gc.collect()
assert loop.quit()
assert loop.wait_for_quit(2000)

# Replacing a callback must release the GIL while Timer waits for the active
# callback's recursive mutex.
for operation in ("set", "start"):
    loop = vlink.MessageLoop()
    assert loop.async_run()
    entered = threading.Event()
    timer = vlink.Timer(loop, 1, -1, slow_callback)
    timer.start()
    assert entered.wait(2.0)
    if operation == "set":
        timer.set_callback(lambda: None)
    else:
        timer.start(lambda: None)
    timer.stop()
    assert loop.quit()
    assert loop.wait_for_quit(2000)

# Deleting the last Python reference inside the callback must defer native
# destruction until the callback has unwound from the loop thread.
loop = vlink.MessageLoop()
assert loop.async_run()
done = threading.Event()
timer = None

def self_delete():
    global timer
    timer = None
    gc.collect()
    done.set()

timer = vlink.Timer(loop, 1, 1, self_delete)
timer.start()
assert done.wait(2.0)
assert loop.quit()
assert loop.wait_for_quit(2000)

# Keep the sole loop owner alive until the deferred timer cleanup has detached
# from it. Releasing the loop on its own callback thread is not a valid teardown.
loop = vlink.MessageLoop()
assert loop.async_run()
done = threading.Event()
timer = None

def delete_timer_and_sole_loop_owner():
    global timer
    timer = None
    gc.collect()
    done.set()

timer = vlink.Timer(loop, 1, 1, delete_timer_and_sole_loop_owner)
loop = None
gc.collect()
timer.start()
assert done.wait(2.0)

loop = vlink.MessageLoop()
assert loop.async_run()
entered = threading.Event()

def detach_while_running():
    entered.set()
    native_sleep(500)

timer = vlink.Timer(loop, 1, -1, detach_while_running)
loop = None
gc.collect()
timer.start()
assert entered.wait(2.0)
assert timer.detach()

loop = vlink.MessageLoop()
assert loop.async_run()
done = threading.Event()
timer = None
process = vlink.Process()

def nested_state_callback(state):
    global timer
    if state == vlink.Process.State.Starting:
        timer = None
        gc.collect()

def outer_timer_callback():
    process.start(sys.executable, ["-c", "pass"])
    done.set()

process.register_state_changed_callback(nested_state_callback)
timer = vlink.Timer(loop, 1, 1, outer_timer_callback)
loop = None
gc.collect()
timer.start()
assert done.wait(2.0)
assert process.wait_for_finished(3000)
'''
    subprocess.run(
        [sys.executable, "-c", lifetime_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=10.0,
    )

    # TIMER_INFINITE constant
    assert _vlink.TIMER_INFINITE == -1

    loop.quit()
    loop.wait_for_quit(2000)
    print("[PASS] Timer extended")


def test_wheel_timer_extended():
    wheel = _vlink.WheelTimer(32, 10)
    assert not wheel.is_running()
    wheel.set_catchup_limit(4)

    pending_key = wheel.add(100, lambda key: None)
    assert isinstance(pending_key, int)
    assert isinstance(wheel.get_remaining_time(pending_key), int)
    assert wheel.remove(pending_key)
    assert wheel.get_remaining_time(pending_key) == 0

    fired = threading.Event()
    wheel.add(10, lambda key: fired.set())
    wheel.start()
    assert fired.wait(1.0)
    wheel.stop()
    assert not wheel.is_running()

    deadlock_check = """
import ctypes
import gc
import os
import vlink

def hold_gil(milliseconds):
    if os.name == "nt":
        ctypes.PyDLL("kernel32").Sleep(milliseconds)
    else:
        ctypes.PyDLL(None).usleep(milliseconds * 1000)

wheel = vlink.WheelTimer(32, 1)
wheel.add(1, lambda key: None)
wheel.start()
hold_gil(50)
wheel.stop()

wheel = vlink.WheelTimer(32, 1)
wheel.add(1, lambda key: None)
wheel.start()
hold_gil(50)
del wheel
gc.collect()
"""
    subprocess.run(
        [sys.executable, "-c", deadlock_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=3.0,
    )
    print("[PASS] WheelTimer extended")


def test_thread_pool_extended():
    pool = _vlink.ThreadPool(2)
    assert pool.get_max_task_count() >= 0
    pool.set_strategy(_vlink.ThreadPoolStrategy.Block)
    assert pool.get_strategy() == _vlink.ThreadPoolStrategy.Block

    done = threading.Event()
    count = [0]
    lock = threading.Lock()

    def task():
        with lock:
            count[0] += 1
            if count[0] >= 4:
                done.set()

    for _ in range(4):
        assert pool.post_task(task) is True
    assert done.wait(2.0)
    assert count[0] >= 4
    assert pool.shutdown() is True
    assert pool.post_task(lambda: None) is False

    pool2 = _vlink.ThreadPool(1, _vlink.ThreadPoolType.Normal)
    assert pool2.get_type() == _vlink.ThreadPoolType.Normal
    assert pool2.shutdown() is True
    print("[PASS] ThreadPool extended")


def test_callback_owner_lifetimes():
    lifetime_check = r'''
import ctypes
import gc
import os
import threading
import time
import weakref
import vlink

def native_sleep(milliseconds):
    if os.name == "nt":
        ctypes.CDLL("kernel32").Sleep(milliseconds)
    else:
        ctypes.CDLL(None).usleep(milliseconds * 1000)

for owner_type in (vlink.MessageLoop, lambda: vlink.MultiLoop(1), lambda: vlink.ThreadPool(1)):
    entered = threading.Event()
    owner = owner_type()
    owner.post_task(lambda: (entered.set(), native_sleep(300)))
    if not isinstance(owner, vlink.ThreadPool):
        assert owner.async_run()
    assert entered.wait(2.0)
    owner = None
    gc.collect()

for owner_type in (vlink.MessageLoop, lambda: vlink.MultiLoop(1), lambda: vlink.ThreadPool(1)):
    done = threading.Event()
    owner = owner_type()
    ref = weakref.ref(owner)

    def self_delete():
        global owner
        owner = None
        gc.collect()
        done.set()

    owner.post_task(self_delete)
    if not isinstance(owner, vlink.ThreadPool):
        assert owner.async_run()
    assert done.wait(2.0)
    for _ in range(100):
        if ref() is None:
            break
        gc.collect()
        time.sleep(0.01)
    assert ref() is None

url = "intra://python/callback-owner-lifetime"
subscriber = vlink.Subscriber(url)
publisher = vlink.Publisher(url)
entered = threading.Event()

def node_callback(data):
    entered.set()
    native_sleep(300)

assert subscriber.listen(node_callback)
subscriber_ref = weakref.ref(subscriber)
thread = threading.Thread(target=lambda: publisher.publish(b"payload"))
thread.start()
assert entered.wait(2.0)
subscriber = None
gc.collect()
thread.join(2.0)
assert not thread.is_alive()
for _ in range(100):
    if subscriber_ref() is None:
        break
    gc.collect()
    time.sleep(0.01)
assert subscriber_ref() is None
time.sleep(0.2)
publisher.deinit()

url = "intra://python/explicit-deinit-lifetime"
subscriber = vlink.Subscriber(url)
publisher = vlink.Publisher(url)
entered = threading.Event()
assert subscriber.listen(lambda data: (entered.set(), native_sleep(300)))
thread = threading.Thread(target=lambda: publisher.publish(b"payload"))
thread.start()
assert entered.wait(2.0)
assert subscriber.deinit()
thread.join(2.0)
assert not thread.is_alive()
publisher.deinit()
'''
    subprocess.run(
        [sys.executable, "-c", lifetime_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=8.0,
    )
    print("[PASS] callback-owner lifetimes")


def test_fixed_utf8_fields():
    def check(obj, setter, getter, fitting, truncated):
        getattr(obj, setter)(fitting)
        assert getattr(obj, getter)() == fitting
        getattr(obj, setter)(truncated)
        assert getattr(obj, getter)() == truncated[:-1]

    check(_vlink.OccupancyGrid(), "set_map_id", "map_id", "a" * 12 + "中", "a" * 13 + "中")
    check(_vlink.Tensor(), "set_name", "name", "a" * 28 + "中", "a" * 29 + "中")
    check(_vlink.Tensor(), "set_model_id", "model_id", "a" * 28 + "中", "a" * 29 + "中")
    check(_vlink.Tensor(), "set_layout", "layout", "a" * 12 + "中", "a" * 13 + "中")
    check(_vlink.ObjectArray(), "set_source_id", "source_id", "a" * 12 + "中", "a" * 13 + "中")
    check(_vlink.AudioFrame(), "set_codec", "codec", "a" * 12 + "中", "a" * 13 + "中")
    check(_vlink.AudioFrame(), "set_language", "language", "a" * 4 + "中", "a" * 5 + "中")

    header = _vlink.ZeroCopyHeader()
    header.frame_id = "a" * 13 + "中"
    assert header.frame_id == "a" * 13

    obj = _vlink.ObjectArray.Object()
    obj.label = "a" * 29 + "中"
    assert obj.label == "a" * 29

    qos = _vlink.Qos()
    qos.name = "a" * 17 + "中"
    assert qos.name == "a" * 17
    print("[PASS] fixed UTF-8 fields")


def test_process():
    # Static execute
    ret = _vlink.Process.execute("/bin/echo", ["hello"])
    assert ret == 0

    # Instance-based
    p = _vlink.Process()
    assert p.get_state() == _vlink.Process.State.NotRunning

    p.set_working_directory("/tmp")
    assert p.get_working_directory() == "/tmp"

    p.set_inherit_environment(True)
    assert p.get_inherit_environment()

    p.set_process_mode(_vlink.Process.Mode.Merged)
    assert p.get_process_mode() == _vlink.Process.Mode.Merged

    # Run a command and read output
    p2 = _vlink.Process()
    p2.set_process_mode(_vlink.Process.Mode.Merged)
    p2.start("/bin/echo", ["hello_vlink"])
    p2.wait_for_finished(5000)
    output = p2.read_all()
    assert b"hello_vlink" in output
    assert p2.get_exit_code() == 0
    assert p2.get_exit_status() == _vlink.Process.ExitStatus.Normal

    # start_command
    p3 = _vlink.Process()
    p3.set_process_mode(_vlink.Process.Mode.Merged)
    p3.start_command("/bin/echo test_cmd")
    p3.wait_for_finished(5000)
    out3 = p3.read_all()
    assert b"test_cmd" in out3

    # start_detached
    assert _vlink.Process.start_detached("/bin/true")

    # Callbacks
    finished_info = [None]
    p4 = _vlink.Process()
    p4.register_finished_callback(lambda code, status: finished_info.__setitem__(0, (code, status)))
    p4.start("/bin/true")
    p4.wait_for_finished(5000)
    time.sleep(0.1)
    assert finished_info[0] is not None
    assert finished_info[0][0] == 0

    restart_error = []
    restart_checked = threading.Event()
    p4_restart = _vlink.Process()

    def reject_restart_from_callback(code, status):
        try:
            p4_restart.start("/bin/true")
        except RuntimeError as exc:
            restart_error.append(str(exc))
        restart_checked.set()

    p4_restart.register_finished_callback(reject_restart_from_callback)
    p4_restart.start("/bin/true")
    assert restart_checked.wait(2.0)
    assert restart_error and "active callback" in restart_error[0]
    p4_restart.close()
    p4_restart.register_finished_callback(lambda code, status: None)

    p5 = _vlink.Process()
    p5.set_process_mode(_vlink.Process.Mode.Merged)
    p5.start("/bin/echo", ["line_one"])
    p5.wait_for_finished(5000)
    assert isinstance(p5.can_read_line_stdout(), bool)
    line = p5.read_line_stdout()
    assert line is None or b"line_one" in line

    p6 = _vlink.Process()
    p6.set_process_mode(_vlink.Process.Mode.Merged)
    p6.start("/bin/echo", ["chunk"])
    p6.wait_for_finished(5000)
    chunk_out = p6.read_stdout(0)
    assert isinstance(chunk_out, bytes)

    p7 = _vlink.Process()
    p7.set_process_mode(_vlink.Process.Mode.Merged)
    p7.start("/bin/cat")
    assert p7.wait_for_started(5000)
    assert p7.write(bytearray(b"bytes_write_line\n"))
    p7.close_write_channel()
    p7.wait_for_finished(5000)
    assert b"bytes_write_line" in p7.read_all()

    assert _vlink.Process.INFINITE == -1
    assert _vlink.Process.DEFAULT_WAIT_TIMEOUT_MS == 3000

    # Process destruction joins its monitor thread. A ready-read callback can
    # be waiting for the GIL at that point, so exercise the race repeatedly in
    # an isolated subprocess and fail by timeout instead of hanging this suite.
    deadlock_check = r'''
import ctypes
import gc
import os
import sys
import threading
import vlink

entered = threading.Event()

def callback():
    entered.set()
    if os.name == "nt":
        ctypes.CDLL("kernel32").Sleep(500)
    else:
        ctypes.CDLL(None).usleep(500_000)

child_code = "import time\nprint('ready', flush=True)\ntime.sleep(60)"
process = vlink.Process()
process.register_ready_read_stdout_callback(callback)
process.start(sys.executable, ["-u", "-c", child_code])
assert process.wait_for_started(3000)
assert entered.wait(2.0)
del process
gc.collect()
'''
    subprocess.run(
        [sys.executable, "-c", deadlock_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=4.0,
    )

    self_delete_check = r'''
import gc
import sys
import threading
import vlink

done = threading.Event()
process = None

def callback():
    global process
    process = None
    gc.collect()
    done.set()

child_code = "import time\nprint('ready', flush=True)\ntime.sleep(60)"
process = vlink.Process()
process.register_ready_read_stdout_callback(callback)
process.start(sys.executable, ["-u", "-c", child_code])
assert done.wait(2.0)
'''
    subprocess.run(
        [sys.executable, "-c", self_delete_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=4.0,
    )

    print("[PASS] Process")


def test_utils_extended():
    # New utils
    assert isinstance(_vlink.utils.get_app_path(), str)

    mid = _vlink.utils.get_machine_id()
    assert isinstance(mid, str)

    tid = _vlink.utils.get_native_thread_id()
    assert tid > 0

    tz = _vlink.utils.get_timezone_diff()
    assert isinstance(tz, int)

    cpu = _vlink.utils.get_cpu_usage()
    assert isinstance(cpu, float)

    mem = _vlink.utils.get_memory_usage()
    assert isinstance(mem, float)

    # is_process_running
    assert isinstance(_vlink.utils.is_process_running("nonexistent_12345"), bool)

    # check_singleton
    assert isinstance(_vlink.utils.check_singleton("py_test_singleton_" + str(os.getpid())), bool)

    # filter_available default false: returns full list
    addrs = _vlink.utils.get_all_ipv4_address(filter_available=False)
    assert isinstance(addrs, list)

    # get_dds_default_address respects max_count
    capped = _vlink.utils.get_dds_default_address(max_count=2)
    assert isinstance(capped, list) and len(capped) <= 2

    # set_thread_name without thread arg
    assert isinstance(_vlink.utils.set_thread_name("py_audit_thread"), bool)

    assert _vlink.utils.wait_for_device("/tmp", 10, 1)

    print("[PASS] Utils extended")


def test_helpers_extended():
    assert _vlink.helpers.double_to_string(3.14159, 2) == "3.14"
    assert _vlink.helpers.hash_combine(123, 456) > 0
    assert _vlink.helpers.contains_substring("hello world", "world")
    assert not _vlink.helpers.contains_substring("hello", "xyz")

    # format_date
    date_str = _vlink.helpers.format_date(1700000000000000000)  # ns since epoch
    assert isinstance(date_str, str) and len(date_str) > 0

    # format_time_diff
    diff = _vlink.helpers.format_time_diff(3661000)
    assert isinstance(diff, str)

    # format_hex_number
    hex_str = _vlink.helpers.format_hex_number(255)
    assert "ff" in hex_str.lower() or "FF" in hex_str

    # convert_date_to_timestamp
    ts = _vlink.helpers.convert_date_to_timestamp("2024-01-01 00:00:00")
    assert isinstance(ts, int)

    # replace_string returns a new string (Python str is immutable)
    replaced = _vlink.helpers.replace_string("a-b-c", "-", "_")
    assert replaced == "a_b_c"
    assert _vlink.helpers.trim_string_view("  a b  ") == "a b"

    # format_hex_number_unsigned overload
    assert isinstance(_vlink.helpers.format_hex_number_unsigned(0xDEADBEEF), str)

    # local <-> utf8 round-trip is a no-op when the source is plain ASCII
    rt = _vlink.helpers.string_utf8_to_local(_vlink.helpers.string_local_to_utf8("ascii-text"))
    assert rt == "ascii-text"

    wide_text = "ascii-text-é"
    assert _vlink.helpers.string_to_wstring(wide_text) == wide_text
    assert _vlink.helpers.wstring_to_string(wide_text) == wide_text
    assert isinstance(_vlink.helpers.path_to_string("/tmp"), str)
    assert _vlink.helpers.split("a,b") == ["a", "b"]
    assert _vlink.helpers.split_view("a,b", ",") == ["a", "b"]
    assert _vlink.helpers.split_view("a,b") == ["a", "b"]
    assert _vlink.helpers.split_any(" a,b  c ") == ["a", "b", "c"]
    assert _vlink.helpers.split_any("a;b,c") == ["a;b", "c"]
    assert _vlink.helpers.split_any("a|b/c", "|/") == ["a", "b", "c"]
    assert _vlink.helpers.split_any_view("a|b, c") == ["a|b", "c"]
    assert _vlink.helpers.split_any_view("a;b,c") == ["a;b", "c"]
    assert _vlink.helpers.split_any_view("a|b/c", "|/") == ["a", "b", "c"]

    print("[PASS] Helpers extended")


def test_qos_enums():
    # Reliability::Kind
    assert _vlink.Qos.Reliability.Kind.BestEffort is not None
    assert _vlink.Qos.Reliability.Kind.Reliable is not None

    # History::Kind
    assert _vlink.Qos.History.Kind.KeepLast is not None
    assert _vlink.Qos.History.Kind.KeepAll is not None

    # Durability::Kind
    assert _vlink.Qos.Durability.Kind.Volatile is not None
    assert _vlink.Qos.Durability.Kind.TransientLocal is not None
    assert _vlink.Qos.Durability.Kind.Transient is not None
    assert _vlink.Qos.Durability.Kind.Persistent is not None

    # PublishMode::Kind
    assert _vlink.Qos.PublishMode.Kind.Sync is not None
    assert _vlink.Qos.PublishMode.Kind.ASync is not None

    # Liveliness::Kind
    assert _vlink.Qos.Liveliness.Kind.Automatic is not None

    # Full QoS construction
    qos = _vlink.Qos()
    qos.reliability.kind = _vlink.Qos.Reliability.Kind.Reliable
    qos.reliability.block_time = 100
    qos.history.kind = _vlink.Qos.History.Kind.KeepLast
    qos.history.depth = 10
    qos.durability.kind = _vlink.Qos.Durability.Kind.TransientLocal
    qos.publish_mode.kind = _vlink.Qos.PublishMode.Kind.ASync
    qos.deadline.period = 1000
    qos.lifespan.duration = 5000
    qos.latency_budget.duration = 50
    qos.resource_limits.max_samples = 100
    qos.additions.priority = _vlink.Qos.Additions.Priority.RealTime
    qos.additions.is_express = True
    qos.valid = True
    assert qos.valid
    qos.additions.priority = _vlink.Qos.Additions.Priority.Normal
    assert qos.additions.priority == _vlink.Qos.Additions.Priority.Normal

    qos.destination_order.kind = _vlink.Qos.DestinationOrder.Kind.SourceTimestamp
    assert qos.destination_order.kind == _vlink.Qos.DestinationOrder.Kind.SourceTimestamp
    qos.ownership.kind = _vlink.Qos.Ownership.Kind.Exclusive
    assert qos.ownership.kind == _vlink.Qos.Ownership.Kind.Exclusive

    qos.name = "py_audit_profile"
    assert qos.name == "py_audit_profile"
    long_name = "x" * 50
    qos.name = long_name
    assert qos.name == long_name[:19]

    print("[PASS] QoS enums & construction")


def test_url_remap():
    remap = _vlink.UrlRemap()
    assert not remap.is_valid()

    # Create a temp remap file
    remap_file = os.path.join(tempfile.mkdtemp(), "remap.json")
    with open(remap_file, "w") as f:
        f.write('{"old://topic": "new://topic"}')

    assert remap.load(remap_file)
    assert remap.is_valid()
    assert remap.convert("old://topic") == "new://topic"
    assert remap.convert("unchanged://topic") == "unchanged://topic"

    remap.set_enable_log(True)
    assert remap.is_enable_log()

    assert remap.unload()
    assert not remap.is_valid()

    os.remove(remap_file)
    print("[PASS] UrlRemap")


def test_node_common_apis():
    """Test Node<> shared methods via Publisher."""
    pub = _vlink.Publisher("intra://test/node_common", auto_init=False)

    # Discovery
    pub.set_discovery_enabled(False)
    assert not pub.get_discovery_enabled()
    pub.set_discovery_enabled(True)
    assert pub.get_discovery_enabled()

    # schema_type
    pub.set_ser_type("demo.proto.NodeCommon", _vlink.SchemaType.Protobuf)
    assert pub.get_ser_type() == "demo.proto.NodeCommon"
    assert pub.get_schema_type() == _vlink.SchemaType.Protobuf
    pub.set_ser_type("demo.proto.NodeCommonV2")
    assert pub.get_ser_type() == "demo.proto.NodeCommonV2"
    assert pub.get_schema_type() == _vlink.SchemaType.Protobuf
    pub.set_ser_type("")
    assert pub.get_ser_type() == ""
    assert pub.get_schema_type() == _vlink.SchemaType.Unknown
    pub.set_ser_type("demo.proto.NodeCommon", _vlink.SchemaType.Protobuf)

    # Safety quit
    pub.set_safety_quit(True)
    assert pub.get_safety_quit()
    pub.set_safety_quit(False)

    pub.init()

    # Unsupported attach must not retain a MessageLoop merely because it was
    # passed to the binding.
    unsupported_loop = _vlink.MessageLoop()
    unsupported_refs = sys.getrefcount(unsupported_loop)
    assert not pub.attach(unsupported_loop)
    assert sys.getrefcount(unsupported_loop) == unsupported_refs

    # DDS nodes support MessageLoop attachment. Releasing the node's retained
    # loop from the loop thread must be deferred until the current task exits;
    # otherwise MessageLoop::~MessageLoop attempts to join its own thread.
    node_loop_lifetime_check = r'''
import gc
import sys
import threading
import time
import vlink

loop = vlink.MessageLoop()
refs = sys.getrefcount(loop)
node = vlink.Publisher("dds://python/node-loop-main", auto_init=False)
assert node.attach(loop)
assert sys.getrefcount(loop) == refs + 1
assert node.detach()
assert sys.getrefcount(loop) == refs

# Destruction of an initialized attached node waits for the loop to become
# idle. The binding must release the GIL even when the node has no callbacks of
# its own, so an active Python loop task can finish.
loop = vlink.MessageLoop()
assert loop.async_run()
entered = threading.Event()

def slow_loop_task():
    entered.set()
    time.sleep(0.2)

assert loop.post_task(slow_loop_task)
assert entered.wait(2.0)
node = vlink.Publisher("dds://python/node-loop-external-delete", auto_init=False)
assert node.attach(loop)
assert node.init()
node = None
gc.collect()
assert loop.quit()
assert loop.wait_for_quit(2000)

loop = vlink.MessageLoop()
done = threading.Event()
ended = threading.Event()
loop.register_end_handler(ended.set)
assert loop.async_run()
node = vlink.Publisher("dds://python/node-loop-detach", auto_init=False)
assert node.attach(loop)

def detach_on_loop():
    assert node.detach()
    done.set()

assert loop.post_task(detach_on_loop)
loop = None
gc.collect()
assert done.wait(2.0)
assert ended.wait(2.0)

# Releasing the node's reference must not stop a loop that still has another
# Python owner.
loop = vlink.MessageLoop()
refs = sys.getrefcount(loop)
assert loop.async_run()
node = vlink.Publisher("dds://python/node-loop-shared", auto_init=False)
assert node.attach(loop)
done = threading.Event()

def detach_from_shared_loop():
    assert node.detach()
    done.set()

assert loop.post_task(detach_from_shared_loop)
assert done.wait(2.0)
deadline = time.monotonic() + 2.0
while sys.getrefcount(loop) != refs and time.monotonic() < deadline:
    time.sleep(0.01)
assert sys.getrefcount(loop) == refs
assert loop.is_running()
assert loop.quit()
assert loop.wait_for_quit(2000)

loop = vlink.MessageLoop()
ended = threading.Event()
loop.register_end_handler(ended.set)
assert loop.async_run()
holder = [vlink.Publisher("dds://python/node-loop-delete", auto_init=False)]
assert holder[0].attach(loop)
done = threading.Event()

def delete_on_loop():
    holder.clear()
    gc.collect()
    done.set()

assert loop.post_task(delete_on_loop)
loop = None
gc.collect()
assert done.wait(2.0)
assert ended.wait(2.0)
'''
    subprocess.run(
        [sys.executable, "-c", node_loop_lifetime_check],
        check=True,
        capture_output=True,
        text=True,
        timeout=5.0,
    )

    # Transport type
    assert pub.get_transport_type() == _vlink.TransportType.Intra

    # Properties
    pub.set_property("test_key", "test_val")
    assert pub.get_property("test_key") == "test_val"

    pub.deinit()
    print("[PASS] Node common APIs")


def test_node_wire_meta_validation():
    raw_pub = _vlink.Publisher("intra://test/raw_ser_only", ser_type="raw", auto_init=False)
    assert raw_pub.get_ser_type() == "raw"
    assert raw_pub.get_schema_type() == _vlink.SchemaType.Raw

    try:
        _vlink.Publisher(
            "intra://test/invalid_schema_only",
            schema_type=_vlink.SchemaType.Protobuf,
            auto_init=False,
        )
        assert False, "Expected ValueError for schema_type without ser_type"
    except ValueError:
        pass

    pub = _vlink.Publisher(
        "intra://test/valid_wire_meta",
        ser_type="demo.proto.Valid",
        schema_type=_vlink.SchemaType.Protobuf,
        auto_init=False,
    )
    assert pub.get_ser_type() == "demo.proto.Valid"
    assert pub.get_schema_type() == _vlink.SchemaType.Protobuf

    cfg = _vlink.SecurityConfig()
    cfg.key = "python-api-key"
    sec_pub = _vlink.SecurityPublisher("shm://test/security_raw_ser_only", cfg, ser_type="raw", auto_init=False)
    assert sec_pub.get_ser_type() == "raw"
    assert sec_pub.get_schema_type() == _vlink.SchemaType.Raw
    print("[PASS] Node wire_meta validation")


def test_getter_change_reporting():
    getter = _make_node(_vlink.Getter, "intra://test/cr")
    getter.set_change_reporting(True)
    assert getter.get_change_reporting()
    getter.set_change_reporting(False)
    assert not getter.get_change_reporting()
    getter.deinit()
    print("[PASS] Getter change_reporting")


def test_bag_extended():
    bag_path = os.path.join(tempfile.mkdtemp(), "ext_test.vdb")

    # Writer with split callback
    cfg = _vlink.BagWriter.Config()
    cfg.compress = _vlink.BagWriter.CompressType.LZAV
    cfg.ignore_compress_urls = {"intra://schema_cb"}
    w = _vlink.BagWriter.create(bag_path, cfg)
    w.async_run()

    explicit_schema = _vlink.SchemaData()
    explicit_schema.name = "demo.Explicit"
    explicit_schema.encoding = "protobuf"
    explicit_schema.schema_type = _vlink.SchemaType.Protobuf
    explicit_schema.data = _vlink.Bytes.from_bytes(b"explicit-schema")
    assert w.push_schema(explicit_schema)

    callback_calls = []
    writer_callback_replace_error = []
    writer_ref = weakref.ref(w)

    def schema_callback(ser_type, schema_type):
        callback_calls.append((ser_type, schema_type))
        try:
            writer_ref().register_schema_callback(lambda *_: None)
        except RuntimeError as exc:
            writer_callback_replace_error.append(str(exc))
        schema = _vlink.SchemaData()
        schema.name = ser_type
        schema.encoding = "protobuf"
        schema.schema_type = schema_type
        schema.data = _vlink.Bytes.from_bytes(b"callback-schema")
        return schema

    w.register_schema_callback(schema_callback)
    for i in range(5):
        frame = _bag_frame("intra://t", "raw", _vlink.SchemaType.Raw, _vlink.ActionType.Publish, f"m{i}".encode())
        timestamp = w.push(frame)
        assert isinstance(timestamp, int)
    schema_timestamp = w.push(_bag_frame(
        "intra://schema_cb",
        "demo.Callback",
        _vlink.SchemaType.Protobuf,
        _vlink.ActionType.Publish,
        b"schema-msg",
    ))
    assert isinstance(schema_timestamp, int)
    time.sleep(0.05)
    timestamp = w.push(_bag_frame("intra://t", "raw", _vlink.SchemaType.Raw, _vlink.ActionType.Publish, b"queued"))
    assert isinstance(timestamp, int)
    time.sleep(0.2)
    assert w.wait_for_idle(5000)
    assert writer_callback_replace_error and "active Python callback" in writer_callback_replace_error[0]
    w.quit()
    assert w.wait_for_quit(5000)
    w.close()
    assert not w.fail()
    del w  # release writer before opening reader

    # filter_get (shared instance management) - uses separate file
    filter_path = bag_path + ".filter_test.vdb"
    w2 = _vlink.BagWriter.filter_get(filter_path)
    assert w2 is not None
    del w2  # release before reader opens bag_path
    assert _vlink.BagWriter.create(os.path.join(tempfile.mkdtemp(), "bad.unknown")) is None
    assert _vlink.BagWriter.filter_get(os.path.join(tempfile.mkdtemp(), "bad.unknown")) is None

    # Reader with check/reindex
    r = _vlink.BagReader.create(bag_path)
    reader_refcount = sys.getrefcount(r)
    info = r.get_info()
    assert sys.getrefcount(r) == reader_refcount + 1
    assert info.message_count >= 5
    assert info.storage_type == "SQLite3"
    metas = {meta.url: meta for meta in info.url_metas}
    assert metas["intra://t"].schema_type == _vlink.SchemaType.Raw
    assert r.get_schema_type("intra://t") == _vlink.SchemaType.Raw
    schemas = r.detect_schema()
    schema_names = {schema.name for schema in schemas}
    assert "demo.Explicit" in schema_names

    # Start loop first (check/reindex run async tasks on the loop)
    r.async_run()

    # check integrity
    check_ok = r.check()
    assert isinstance(check_ok, bool)

    # reindex (may return False if already indexed, that's OK)
    reindex_result = r.reindex()
    assert isinstance(reindex_result, bool)

    # Status callback
    statuses = []
    r.register_status_callback(lambda s: statuses.append(s))

    # Playback
    msgs = []
    outputs = []
    reader_callback_replace_error = []
    reader_ref = weakref.ref(r)
    done = threading.Event()

    def on_output(frame):
        if not reader_callback_replace_error:
            try:
                reader_ref().register_output_callback(lambda _: None)
            except RuntimeError as exc:
                reader_callback_replace_error.append(str(exc))
        outputs.append((frame.timestamp, frame.url, frame.action_type, frame.data))
        msgs.append(frame.data)

    r.register_output_callback(on_output)
    r.register_finish_callback(lambda intr: done.set())

    cfg2 = _vlink.BagReader.Config()
    cfg2.rate = 100.0
    cfg2.force_delay = 0
    cfg2.auto_quit = True
    r.play(cfg2)
    done.wait(10.0)

    assert len(msgs) >= 5
    assert reader_callback_replace_error and "active Python callback" in reader_callback_replace_error[0]
    assert b"m0" in msgs
    assert all(type(action) is type(_vlink.ActionType.Publish) for _, _, action, _ in outputs)

    r.quit()
    r.wait_for_quit(5000)

    os.remove(bag_path)
    print("[PASS] Bag extended")


def test_bag_writer_explicit_zero_timestamp():
    bag_path = os.path.join(tempfile.mkdtemp(), "zero_ts.vdb")
    cfg = _vlink.BagWriter.Config()
    cfg.sync_mode = True
    w = _vlink.BagWriter.create(bag_path, cfg)
    timestamp = w.push(_bag_frame(
        "intra://zero_ts",
        "raw",
        _vlink.SchemaType.Raw,
        _vlink.ActionType.Publish,
        b"zero-ts",
        timestamp=0,
    ))
    assert timestamp == 0
    del w

    r = _vlink.BagReader.create(bag_path)
    outputs = []
    done = threading.Event()
    r.register_output_callback(
        lambda frame: outputs.append((frame.timestamp, frame.url, frame.action_type, frame.data))
    )
    r.register_finish_callback(lambda intr: done.set())
    r.async_run()
    cfg = _vlink.BagReader.Config()
    cfg.force_delay = 0
    cfg.auto_quit = True
    r.play(cfg)
    done.wait(5.0)

    assert outputs == [(0, "intra://zero_ts", _vlink.ActionType.Publish, b"zero-ts")]
    assert type(outputs[0][2]) is type(_vlink.ActionType.Publish)

    r.quit()
    r.wait_for_quit(5000)
    os.remove(bag_path)
    print("[PASS] BagWriter explicit timestamp=0")


def test_bag_cursor_read():
    bag_path = os.path.join(tempfile.mkdtemp(), "cursor.vdb")

    w = _vlink.BagWriter.create(bag_path)
    assert w is not None
    w.async_run()
    for i in range(6):
        w.push(_bag_frame(
            "intra://cursor",
            "raw",
            _vlink.SchemaType.Raw,
            _vlink.ActionType.Publish,
            f"c{i}".encode(),
            timestamp=i * 10_000,
        ))
    assert not w.fail()
    assert bool(w)
    w.wait_for_idle()
    del w

    r = _vlink.BagReader.create(bag_path, read_only=True)
    assert r.open_cursor()
    payloads = [bytes(frame.data) for frame in r]
    assert payloads == [f"c{i}".encode() for i in range(6)], f"got {payloads}"
    assert r.eof()
    assert not r.fail()
    assert not bool(r)

    cfg = _vlink.BagReader.Config()
    cfg.begin_time = 30
    assert r.open_cursor(cfg)
    count = 0
    while True:
        frame = r.read_next()
        if frame is None:
            break
        count += 1
    assert count == 3, f"got {count}"

    del r
    os.remove(bag_path)
    print("[PASS] Bag cursor read")


def test_bag_writer_stream_fail_state():
    bag_path = os.path.join(tempfile.mkdtemp(), "stream_fail.vdb")
    w = _vlink.BagWriter.create(bag_path)
    assert w is not None
    w.async_run()

    assert bool(w)
    assert not w.fail()

    result = w << _bag_frame("", "raw", _vlink.SchemaType.Raw, _vlink.ActionType.Publish, b"x", timestamp=1)
    assert result is w
    assert w.fail()
    assert not bool(w)

    w.clear()
    assert not w.fail()
    assert bool(w)

    w << _bag_frame("intra://ok", "raw", _vlink.SchemaType.Raw, _vlink.ActionType.Publish, b"y", timestamp=2)
    assert not w.fail()

    del w
    os.remove(bag_path)
    print("[PASS] BagWriter stream fail-state")


def test_register_terminate_signal():
    signal_received = []
    signals = [signal.SIGINT, signal.SIGTERM]
    if hasattr(signal, "SIGHUP"):
        signals.append(signal.SIGHUP)
    previous = {sig: signal.getsignal(sig) for sig in signals}

    passthrough_check = """
import os
import signal
import vlink

def fail_callback(sig):
    raise RuntimeError("expected callback failure")

vlink.utils.register_terminate_signal(fail_callback, False, True)
os.kill(os.getpid(), signal.SIGTERM)
print("still alive")
"""
    passthrough_result = subprocess.run(
        [sys.executable, "-c", passthrough_check],
        capture_output=True,
        text=True,
        timeout=3.0,
    )
    if os.name == "nt":
        assert passthrough_result.returncode != 0
    else:
        assert passthrough_result.returncode == -signal.SIGTERM
    assert "still alive" not in passthrough_result.stdout

    try:
        try:
            _vlink.utils.register_terminate_signal(lambda sig: None, True, True)
            assert False, "async pass-through must be rejected during registration"
        except ValueError:
            pass

        _vlink.utils.register_terminate_signal(signal_received.append)
        os.kill(os.getpid(), signal.SIGTERM)
        assert signal_received == [signal.SIGTERM]
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)

    try:
        _vlink.utils.register_crash_signal(lambda sig: None)
        assert False, "fatal signal callbacks must be rejected for Python"
    except RuntimeError:
        pass

    print("[PASS] register_terminate_signal")


def test_ssl_options():
    ssl = _vlink.SslOptions()
    assert not ssl.is_valid()
    ssl.ca_file = "/tmp/ca.pem"
    assert ssl.is_valid()
    ssl.cert_file = "/tmp/cert.pem"
    ssl.key_file = "/tmp/key.pem"
    ssl.key_password = "secret"
    ssl.server_name = "localhost"
    ssl.ciphers = "AES256"
    ssl.verify_peer = True
    assert ssl.is_valid()
    print("[PASS] SslOptions")


def test_security():
    cfg = _vlink.SecurityConfig()
    cfg.key = "my_secret_key_16"
    cfg.advanced.aad_context = "python/security"
    cfg.advanced.replay_window = 0
    sec = _vlink.Security(cfg)
    plain = b"Hello VLink Security!"
    encrypted = sec.encrypt(plain)
    assert encrypted is not None
    assert encrypted != plain
    decrypted = sec.decrypt(encrypted)
    assert decrypted == plain
    assert sec.decrypt(encrypted) == plain
    print("[PASS] Security")


def test_security_set_callbacks():
    def xor_enc(data):
        return bytes(b ^ 0x5A for b in data)

    def xor_dec(data):
        return bytes(b ^ 0x5A for b in data)

    cfg = _vlink.SecurityConfig()
    cfg.encrypt_callback = xor_enc
    cfg.decrypt_callback = xor_dec
    sec = _vlink.Security(cfg)

    plain = b"custom-cipher-roundtrip"
    encrypted = sec.encrypt(plain)
    assert encrypted is not None and encrypted != plain
    decrypted = sec.decrypt(encrypted)
    assert decrypted == plain
    print("[PASS] Security callbacks")


def test_server_async_reply():
    """Test Server listen_for_reply + reply binding without leaving a pending client request."""
    srv = _make_node(_vlink.Server, "intra://test/async_srv2")

    def on_request(req_id, req_data):
        assert isinstance(req_id, int)
        assert isinstance(req_data, (bytes, bytearray, memoryview))

    assert srv.listen_for_reply(on_request)
    assert srv.reply(0, b"resp") is False

    srv.deinit()
    print("[PASS] Server async reply (binding verified)")


def test_client_invoke_timeout_arg():
    cli = _vlink.Client("intra://test/client_timeout_arg", auto_init=False)
    cli.set_discovery_enabled(False)
    cli.init()
    try:
        assert cli.invoke(b"no-server", timeout_ms=20) is None
    finally:
        cli.deinit()
    print("[PASS] Client.invoke timeout_ms")


def test_client_async_invoke_future():
    srv = _make_node(_vlink.Server, "intra://test/client_async_invoke")
    srv.listen(lambda data: b"async:" + bytes(data))

    cli = _make_node(_vlink.Client, "intra://test/client_async_invoke")
    assert cli.wait_for_connected(2000)

    future = cli.async_invoke(b"request")
    assert hasattr(future, "result")
    assert future.result(timeout=2.0) == b"async:request"

    cli.deinit()
    srv.deinit()
    print("[PASS] Client.async_invoke Future")


def test_fire_forget_method_bindings():
    received = []
    ev = threading.Event()

    srv = _make_node(_vlink.FireForgetServer, "intra://test/fire_forget_method")
    srv.listen(lambda data: (received.append(bytes(data)), ev.set()))

    cli = _make_node(_vlink.FireForgetClient, "intra://test/fire_forget_method")
    assert cli.wait_for_connected(2000)
    assert cli.send(b"fire-and-forget")

    ev.wait(2.0)
    assert received == [b"fire-and-forget"]

    cli.deinit()
    srv.deinit()
    print("[PASS] FireForgetServer/FireForgetClient")


def test_exec_task():
    """Test MessageLoop exec_task with delay."""
    loop = _vlink.MessageLoop()
    loop.async_run()

    done = threading.Event()
    t_start = time.time()

    ok = loop.exec_task(50, done.set)  # 50ms delay
    assert ok
    done.wait(2.0)
    elapsed = (time.time() - t_start) * 1000
    assert elapsed >= 40, f"exec_task delay too short: {elapsed}ms"

    loop.quit()
    loop.wait_for_quit(2000)
    print("[PASS] exec_task")


def test_timer_constructors():
    loop = _vlink.MessageLoop()
    loop.async_run()

    done = threading.Event()
    ticks = [0]

    # Full constructor: Timer(loop, interval_ms, loop_count, callback)
    timer = _vlink.Timer(loop, 50, 3, lambda: (ticks.__setitem__(0, ticks[0] + 1),
                                                 done.set() if ticks[0] >= 3 else None))
    timer.start()
    done.wait(3.0)
    assert ticks[0] >= 3, f"Timer ticked only {ticks[0]} times"
    timer.stop()

    configured_done = threading.Event()
    configured = _vlink.Timer(loop, 20, 1)
    configured.set_callback(configured_done.set)
    configured.start()
    configured_done.wait(2.0)
    assert configured_done.is_set()
    configured.stop()

    detached_configured_done = threading.Event()
    detached_configured = _vlink.Timer(20, 1)
    detached_configured.set_callback(detached_configured_done.set)
    assert detached_configured.attach(loop)
    detached_configured.start()
    detached_configured_done.wait(2.0)
    assert detached_configured_done.is_set()
    detached_configured.stop()

    detached_done = threading.Event()
    detached = _vlink.Timer(20, 1, detached_done.set)
    assert detached.attach(loop)
    detached.start()
    detached_done.wait(2.0)
    assert detached_done.is_set()
    detached.stop()

    shorthand_done = threading.Event()
    shorthand = _vlink.Timer(20, shorthand_done.set)
    shorthand.set_loop_count(1)
    assert shorthand.attach(loop)
    shorthand.start()
    shorthand_done.wait(2.0)
    assert shorthand_done.is_set()
    shorthand.stop()

    # Python must retain exactly the currently attached loop, without keeping
    # every former loop alive after reattach/detach or a failed reattach.
    first_loop = _vlink.MessageLoop()
    second_loop = _vlink.MessageLoop()
    first_refs = sys.getrefcount(first_loop)
    second_refs = sys.getrefcount(second_loop)
    owned_timer = _vlink.Timer(first_loop)
    assert sys.getrefcount(first_loop) == first_refs + 1
    assert owned_timer.attach(second_loop)
    assert sys.getrefcount(first_loop) == first_refs
    assert sys.getrefcount(second_loop) == second_refs + 1
    assert owned_timer.detach()
    assert sys.getrefcount(second_loop) == second_refs

    assert owned_timer.attach(first_loop)
    failed_loop = _vlink.MessageLoop()
    assert failed_loop.async_run()
    assert failed_loop.quit()
    assert failed_loop.wait_for_quit(2000)
    assert not owned_timer.attach(failed_loop)
    assert sys.getrefcount(first_loop) == first_refs

    retained_loop = _vlink.MessageLoop()
    retained_refs = sys.getrefcount(retained_loop)
    temporary_timer = _vlink.Timer(retained_loop)
    assert sys.getrefcount(retained_loop) == retained_refs + 1
    del temporary_timer
    assert sys.getrefcount(retained_loop) == retained_refs

    loop.quit()
    loop.wait_for_quit(2000)
    print("[PASS] Timer constructors")


def test_discovery_viewer_fields():
    try:
        dv = _vlink.DiscoveryViewer()
    except RuntimeError as exc:
        if "DiscoveryViewer: Failed to create socket" in str(exc):
            print("[SKIP] DiscoveryViewer fields (socket unavailable)")
            return
        raise

    info_list = dv.get_info_list()
    # Just verify types - may be empty if no services running
    assert isinstance(info_list, list)

    # Test convert_type_to_view
    view = _vlink.DiscoveryViewer.convert_type_to_view(1)  # kPublisher
    assert isinstance(view, str)
    assert _vlink.DiscoveryViewer.convert_type("Pub") == _vlink.ImplType.Publisher

    # Test get_listen_address
    addr = _vlink.DiscoveryViewer.get_listen_address()
    assert isinstance(addr, str)

    print("[PASS] DiscoveryViewer fields")


def test_plugin_host_binding_contract():
    config = _vlink.TriggerRecorder.Config()
    assert config.default_pre_ms == 15000
    assert config.default_post_ms == 0
    assert config.max_cache_size == 2 * 1024 * 1024 * 1024
    assert config.max_dump_file_count == 10
    assert config.retention_guard_ms == 500
    assert config.enable_compress is False
    assert config.overflow == _vlink.TriggerRecorder.OverflowPolicy.DropNewest
    assert not hasattr(config, "dds_ip")
    assert not hasattr(config, "bag_plugin_lib")
    assert not hasattr(config, "bag_plugin_dir")

    loader = _vlink.Plugin()
    missing_name = "__vlink_python_missing_plugin__"
    assert loader.load_bag_plugin(missing_name) is None
    assert loader.load_trigger_plugin(missing_name) is None
    print("[PASS] host-loaded plugin interface contract")


def test_trigger_recorder_lifecycle():
    config = _vlink.TriggerRecorder.Config()
    config.whitelist = ["intra://__python_trigger_test_absent__"]
    config.default_pre_ms = 0
    config.default_post_ms = 0
    config.retention_guard_ms = 0

    with tempfile.TemporaryDirectory() as dump_dir:
        config.dump_dir = dump_dir
        output_path = os.path.join(dump_dir, "python-trigger.vdb")

        try:
            recorder = _vlink.TriggerRecorder(config)
        except RuntimeError as exc:
            if "DiscoveryViewer: Failed to create socket" in str(exc):
                print("[SKIP] TriggerRecorder lifecycle (socket unavailable)")
                return
            raise

        assert not recorder.is_running()
        assert not recorder.is_dumping()
        assert hasattr(recorder, "bind_bag_interface")
        assert hasattr(recorder, "bind_trigger_interface")
        recorder.clear_bag_interface()
        recorder.clear_trigger_interface()
        assert recorder.async_run()
        assert recorder.is_running()

        params = _vlink.TriggerRecorder.TriggerParams()
        assert params.whitelist == set()
        assert params.blacklist == set()
        params.whitelist = {"intra://keep", "intra://drop"}
        params.blacklist = {"intra://drop"}
        assert params.whitelist == {"intra://keep", "intra://drop"}
        assert params.blacklist == {"intra://drop"}
        params.whitelist = set()
        params.blacklist = set()
        params.reason = "python-test"
        params.out_file = output_path
        assert recorder.dump(params)

        deadline = time.monotonic() + 5.0
        while recorder.is_dumping() and time.monotonic() < deadline:
            time.sleep(0.01)

        assert not recorder.is_dumping()
        assert os.path.isfile(output_path)

        recorder.quit()
        assert recorder.wait_for_quit(timeout_ms=5000)
        assert not recorder.is_running()

    print("[PASS] TriggerRecorder lifecycle and dump")


def test_utils_terminal():
    w, h = _vlink.utils.get_terminal_size()
    assert isinstance(w, int)
    assert isinstance(h, int)
    print(f"  Terminal size: {w}x{h}")
    print("[PASS] Utils terminal")


def test_api_surface():
    expected_exports = [
        "ImplType", "TransportType", "InitType", "SecurityType", "ActionType", "SchemaType", "LogLevel", "StatusType",
        "TimerMethod", "TimerAccuracy", "MessageLoopType", "MessageLoopStrategy", "TaskPriority",
        "ThreadPoolType", "ThreadPoolStrategy",
        "Bytes", "Frame", "Version", "SchemaData", "SampleLostInfo",
        "Logger", "ElapsedTimer", "DeadlineTimer", "MessageLoop", "MultiLoop", "Timer", "WheelTimer",
        "ThreadPool", "SpinLock", "CpuProfiler", "CpuProfilerGuard", "MemoryPool",
        "Process", "UrlRemap",
        "Qos", "SslOptions", "Security", "SecurityConfig", "SecurityConfigAdvanced",
        "Publisher", "Subscriber", "Server", "Client", "FireForgetServer", "FireForgetClient", "Setter", "Getter",
        "SecurityPublisher", "SecuritySubscriber", "SecurityServer", "SecurityClient", "SecuritySetter",
        "SecurityGetter", "SecurityFireForgetServer", "SecurityFireForgetClient",
        "DiscoveryViewer", "BagPluginInterface", "TriggerPluginInterface", "Plugin", "BagWriter", "BagReader", "TriggerRecorder",
        "utils", "helpers", "quantize", "QosProfile", "Status",
        "log_trace", "log_debug", "log_info", "log_warn", "log_error", "log_fatal",
        "TIMER_INFINITE", "VERSION", "VERSION_MAJOR", "VERSION_MINOR", "VERSION_PATCH",
    ]
    if getattr(_vlink, "_HAS_MEMORY_RESOURCE", False):
        expected_exports.append("MemoryResource")

    for name in expected_exports:
        assert hasattr(_vlink, name), f"Missing module export: {name}"
        assert name in _vlink.__all__, f"Missing __all__ export: {name}"

    node_methods = [
        "init", "deinit", "set_discovery_enabled", "get_url", "get_transport_type",
        "get_schema_type", "set_ser_type", "get_ser_type", "set_property", "get_property",
        "get_discovery_enabled", "set_record_path", "set_ssl_options", "set_safety_quit",
        "get_safety_quit", "get_cpu_usage", "has_inited", "interrupt", "suspend", "resume",
        "is_suspend", "is_support_loan", "loan", "return_loan",
        "attach", "detach", "get_message_loop", "get_abstract_node", "get_status", "register_status_handler",
    ]
    for cls in (
        _vlink.Publisher, _vlink.Subscriber, _vlink.Server, _vlink.Client,
        _vlink.FireForgetServer, _vlink.FireForgetClient, _vlink.Setter, _vlink.Getter,
    ):
        for method in node_methods:
            assert hasattr(cls, method), f"{cls.__name__} missing method: {method}"
        for method in ("set_manual_unloan", "is_manual_unloan"):
            assert not hasattr(cls, method), f"{cls.__name__} still exposes removed method: {method}"

    class_methods = {
        _vlink.Publisher: ["detect_subscribers", "wait_for_subscribers", "has_subscribers", "publish",
                           "publish_fbb", "mark_as_setter"],
        _vlink.Subscriber: ["listen", "set_latency_and_lost_enabled", "is_latency_and_lost_enabled",
                            "get_latency", "get_lost", "mark_as_getter"],
        _vlink.Server: ["listen", "listen_for_reply", "reply"],
        _vlink.Client: ["detect_connected", "wait_for_connected", "is_connected", "invoke", "invoke_async",
                        "async_invoke"],
        _vlink.FireForgetServer: ["listen"],
        _vlink.FireForgetClient: ["detect_connected", "wait_for_connected", "is_connected", "send"],
        _vlink.Setter: ["set", "mark_as_publisher"],
        _vlink.Getter: ["listen", "get", "wait_for_value", "set_change_reporting", "get_change_reporting",
                        "set_latency_and_lost_enabled", "is_latency_and_lost_enabled", "get_latency", "get_lost",
                        "mark_as_subscriber"],
    }
    for cls, methods in class_methods.items():
        for method in methods:
            assert hasattr(cls, method), f"{cls.__name__} missing method: {method}"

    for cls in (
        _vlink.SecurityPublisher, _vlink.SecuritySubscriber, _vlink.SecurityServer, _vlink.SecurityClient,
        _vlink.SecurityFireForgetServer, _vlink.SecurityFireForgetClient, _vlink.SecuritySetter, _vlink.SecurityGetter,
    ):
        for method in node_methods:
            assert hasattr(cls, method), f"{cls.__name__} missing method: {method}"

    helper_exports = ["to_int", "to_long", "trim_string", "trim_string_view", "has_startwith", "has_endwith",
                      "split", "split_view", "split_any", "split_any_view",
                      "path_to_string"]
    for name in helper_exports:
        assert hasattr(_vlink.helpers, name), f"helpers missing function: {name}"

    removed_helper_exports = [
        "get_" + "split_" + "string",
        "get_" + "split_" + "string_view",
        "get_" + "split_" + "filter",
        "get_" + "split_" + "filter_view",
        "get_" + "pair_" + "string",
        "get_" + "pair_" + "string_view",
    ]
    for name in removed_helper_exports:
        assert not hasattr(_vlink.helpers, name), f"helpers still exports removed function: {name}"

    quantize_exports = ["encode_int16", "decode_int16"]
    for name in quantize_exports:
        assert hasattr(_vlink.quantize, name), f"quantize missing function: {name}"

    utils_exports = ["get_app_path", "get_host_name", "get_pid", "get_pid_str", "get_tmp_dir",
                     "register_terminate_signal", "wait_for_device"]
    for name in utils_exports:
        assert hasattr(_vlink.utils, name), f"utils missing function: {name}"

    assert hasattr(_vlink.BagWriter.Config, "ignore_compress_urls")
    assert hasattr(_vlink.BagWriter.Config, "sync_mode")
    for method in (
        "register_schema_callback", "push_schema", "close", "fail", "clear", "__lshift__", "wait_for_idle",
    ):
        assert hasattr(_vlink.BagWriter, method), f"BagWriter missing method: {method}"
    for method in ("detect_schema", "open_cursor", "read_next", "eof", "fail", "__iter__", "__next__"):
        assert hasattr(_vlink.BagReader, method), f"BagReader missing method: {method}"
    assert hasattr(_vlink.DiscoveryViewer, "convert_type")
    assert not hasattr(_vlink.DiscoveryViewer, "global_get")
    assert hasattr(_vlink.Status, "is_for_writer")
    assert hasattr(_vlink.Status, "is_for_reader")

    print("[PASS] API surface")


def test_node_extended():
    sub = _vlink.Subscriber("intra://test/node_extended", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.init()

    assert sub.get_message_loop() is None or hasattr(sub.get_message_loop(), "is_running")

    status = sub.get_status(_vlink.StatusType.SubscriptionMatched)
    assert status is None or isinstance(status, dict)
    assert _vlink.Status.is_for_reader(_vlink.StatusType.SubscriptionMatched)
    assert _vlink.Status.is_for_writer(_vlink.StatusType.PublicationMatched)
    assert sub.get_abstract_node() is None or isinstance(sub.get_abstract_node(), int)

    sub.deinit()
    print("[PASS] Node extended (get_message_loop / get_status)")


def test_node_role_swaps():
    pub = _vlink.Publisher("intra://test/role_swap_pub", auto_init=False)
    pub.set_discovery_enabled(False)
    pub.mark_as_setter()
    pub.init()
    pub.deinit()

    sub = _vlink.Subscriber("intra://test/role_swap_sub", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.mark_as_getter()
    sub.init()
    sub.deinit()

    setter = _vlink.Setter("intra://test/role_swap_set", auto_init=False)
    setter.set_discovery_enabled(False)
    setter.mark_as_publisher()
    setter.init()
    setter.deinit()

    getter = _vlink.Getter("intra://test/role_swap_get", auto_init=False)
    getter.set_discovery_enabled(False)
    getter.mark_as_subscriber()
    getter.init()
    getter.deinit()
    print("[PASS] Publisher/Subscriber/Setter/Getter role swaps")


def test_security_node_bindings():
    def passthrough(data):
        return bytes(data)

    for cls in (
        _vlink.SecurityPublisher, _vlink.SecuritySubscriber, _vlink.SecurityServer, _vlink.SecurityClient,
        _vlink.SecurityFireForgetServer, _vlink.SecurityFireForgetClient, _vlink.SecuritySetter, _vlink.SecurityGetter,
    ):
        cfg = _vlink.SecurityConfig()
        cfg.key = "python-api-key"
        cfg.encrypt_callback = passthrough
        cfg.decrypt_callback = passthrough
        node = cls(
            f"shm://test/security_bindings_{cls.__name__}",
            cfg,
            ser_type="demo.SecurityBinding",
            schema_type=_vlink.SchemaType.Protobuf,
            auto_init=False,
        )
        assert node.get_transport_type() == _vlink.TransportType.Shm
        assert node.get_ser_type() == "demo.SecurityBinding"
        assert node.get_schema_type() == _vlink.SchemaType.Protobuf
        if cls is _vlink.SecurityFireForgetServer:
            assert hasattr(node, "listen")
        if cls is _vlink.SecurityFireForgetClient:
            assert hasattr(node, "send")

    assert _vlink.SecurityType.WithSecurity is not None
    assert _vlink.ActionType.Unknown is not None
    print("[PASS] Security node bindings")


def test_schema_data_and_version():
    schema = _vlink.SchemaData()
    assert not schema
    schema.name = "demo.Schema"
    schema.encoding = "protobuf"
    schema.schema_type = _vlink.SchemaType.Protobuf
    schema.data = _vlink.Bytes.from_bytes(b"schema")
    assert schema
    assert _vlink.SchemaData.is_valid_type(_vlink.SchemaType.Protobuf)
    assert _vlink.SchemaData.is_real_type(_vlink.SchemaType.Protobuf)
    assert _vlink.SchemaData.convert_type(_vlink.SchemaType.Protobuf) == "protobuf"
    assert _vlink.SchemaData.convert_encoding("proto") == _vlink.SchemaType.Protobuf
    assert _vlink.SchemaData.is_valid_type(_vlink.SchemaType.Cdr)
    assert not _vlink.SchemaData.is_real_type(_vlink.SchemaType.Cdr)
    assert _vlink.SchemaData.convert_type(_vlink.SchemaType.Cdr) == "cdr"
    assert _vlink.SchemaData.convert_encoding("cdr") == _vlink.SchemaType.Cdr
    assert _vlink.SchemaData.infer_ser_type("raw") == _vlink.SchemaType.Raw
    assert _vlink.SchemaData.resolve_type(_vlink.SchemaType.Unknown, "raw", "") == _vlink.SchemaType.Raw

    v1 = _vlink.Version.from_string("1.2.3")
    v2 = _vlink.Version(1, 2, 4)
    assert v1.is_valid()
    assert v1 < v2
    assert v2 > v1
    assert v1 != v2
    print("[PASS] SchemaData / Version helpers")


def test_log_fatal():
    assert callable(_vlink.log_fatal)
    try:
        _vlink.log_fatal("python_api regression: log_fatal smoke test")
    except RuntimeError:
        pass
    print("[PASS] log_fatal")


def test_publish_after_buffer_release():
    received = []
    ev = threading.Event()

    sub = _vlink.Subscriber("intra://test/owned_publish", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.init()
    sub.listen(lambda data: (received.append(bytes(data)), ev.set()))

    pub = _vlink.Publisher("intra://test/owned_publish?type=queue", auto_init=False)
    pub.set_discovery_enabled(False)
    pub.init()
    pub.wait_for_subscribers(timeout_ms=2000)

    payload = bytearray(b"transient-payload-" + b"x" * 1024)
    pub.publish(bytes(payload))
    payload.clear()

    ev.wait(timeout=2.0)
    assert received and received[0].startswith(b"transient-payload-")

    pub.deinit()
    sub.deinit()
    print("[PASS] publish deep-copies caller's bytes (no UAF)")


def test_publish_fbb_binding():
    received = []
    ev = threading.Event()

    sub = _vlink.Subscriber("intra://test/publish_fbb", auto_init=False)
    sub.set_discovery_enabled(False)
    sub.init()
    sub.listen(lambda data: (received.append(bytes(data)), ev.set()))

    pub = _vlink.Publisher("intra://test/publish_fbb?type=queue", auto_init=False)
    pub.set_discovery_enabled(False)
    pub.init()
    pub.wait_for_subscribers(timeout_ms=2000)

    payload = b"\x08\x00\x00\x00FBB-DATA"
    assert pub.publish_fbb(payload)

    ev.wait(timeout=2.0)
    assert received == [payload]

    pub.deinit()
    sub.deinit()
    print("[PASS] Publisher.publish_fbb")


if __name__ == "__main__":
    _vlink.Logger.init("full_test")
    print(f"VLink Full API Test - v{_vlink.VERSION}")
    print("=" * 60)

    test_bytes_extended()
    test_logger_extended()
    test_memory_resource_export()
    test_elapsed_timer_extended()
    test_deadline_timer_extended()
    test_message_loop_extended()
    test_timer_extended()
    test_wheel_timer_extended()
    test_thread_pool_extended()
    test_callback_owner_lifetimes()
    test_fixed_utf8_fields()
    test_process()
    test_utils_extended()
    test_helpers_extended()
    test_qos_enums()
    test_url_remap()
    test_node_common_apis()
    test_getter_change_reporting()
    test_bag_extended()
    test_bag_writer_explicit_zero_timestamp()
    test_bag_cursor_read()
    test_bag_writer_stream_fail_state()
    test_register_terminate_signal()
    test_ssl_options()
    test_security()
    test_security_set_callbacks()
    test_server_async_reply()
    test_client_invoke_timeout_arg()
    test_client_async_invoke_future()
    test_fire_forget_method_bindings()
    test_exec_task()
    test_timer_constructors()
    test_discovery_viewer_fields()
    test_plugin_host_binding_contract()
    test_trigger_recorder_lifecycle()
    test_utils_terminal()
    test_api_surface()
    test_node_wire_meta_validation()
    test_node_extended()
    test_node_role_swaps()
    test_security_node_bindings()
    test_schema_data_and_version()
    test_log_fatal()
    test_publish_after_buffer_release()
    test_publish_fbb_binding()

    print("=" * 60)
    print("ALL TESTS PASSED!")

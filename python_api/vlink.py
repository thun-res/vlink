"""
VLink nanobind wrapper.
"""

from _vlink_nanobind import *  # noqa: F401,F403
from _vlink_nanobind import (
    ImplType, TransportType, InitType, SecurityType, ActionType, SchemaType, LogLevel, StatusType,
    TimerMethod, TimerAccuracy, MessageLoopType, MessageLoopStrategy, TaskPriority,
    ThreadPoolType, ThreadPoolStrategy,
    Bytes, Frame, Uuid, Version, SchemaData, SampleLostInfo,
    ZeroCopyHeader, RawData, CameraFrame, PointCloud, ProxyData,
    OccupancyGrid, Tensor, ObjectArray, AudioFrame, ZeroCopyMessageParser,
    Logger, ElapsedTimer, DeadlineTimer, MessageLoop, MultiLoop, Timer, WheelTimer,
    ThreadPool, SpinLock, CpuProfiler, CpuProfilerGuard, MemoryPool,
    Process, UrlRemap,
    Qos, SslOptions, Security, SecurityConfig, SecurityConfigAdvanced,
    Publisher, Subscriber, Server, Client, FireForgetServer, FireForgetClient, Setter, Getter,
    SecurityPublisher, SecuritySubscriber, SecurityServer, SecurityClient, SecuritySetter, SecurityGetter,
    SecurityFireForgetServer, SecurityFireForgetClient,
    DiscoveryViewer, BagPluginInterface, TriggerPluginInterface, Plugin, BagWriter, BagReader, TriggerRecorder,
    utils, helpers, quantize, QosProfile, Status,
    log_trace, log_debug, log_info, log_warn, log_error, log_fatal,
    TIMER_INFINITE, VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH,
)

try:
    from _vlink_nanobind import MemoryResource  # noqa: F401
    _HAS_MEMORY_RESOURCE = True
except ImportError:
    _HAS_MEMORY_RESOURCE = False

__backend__ = "nanobind"
__version__ = VERSION
__all__ = [
    "ImplType", "TransportType", "InitType", "SecurityType", "ActionType", "SchemaType", "LogLevel", "StatusType",
    "TimerMethod", "TimerAccuracy", "MessageLoopType", "MessageLoopStrategy", "TaskPriority",
    "ThreadPoolType", "ThreadPoolStrategy",
    "Bytes", "Frame", "Uuid", "Version", "SchemaData", "SampleLostInfo",
    "ZeroCopyHeader", "RawData", "CameraFrame", "PointCloud", "ProxyData",
    "OccupancyGrid", "Tensor", "ObjectArray", "AudioFrame", "ZeroCopyMessageParser",
    "Logger", "ElapsedTimer", "DeadlineTimer", "MessageLoop", "MultiLoop", "Timer", "WheelTimer",
    "ThreadPool", "SpinLock", "CpuProfiler", "CpuProfilerGuard", "MemoryPool",
    "Process", "UrlRemap",
    "Qos", "SslOptions", "Security", "SecurityConfig", "SecurityConfigAdvanced",
    "Publisher", "Subscriber", "Server", "Client", "FireForgetServer", "FireForgetClient", "Setter", "Getter",
    "SecurityPublisher", "SecuritySubscriber", "SecurityServer", "SecurityClient", "SecuritySetter", "SecurityGetter",
    "SecurityFireForgetServer", "SecurityFireForgetClient",
    "DiscoveryViewer", "BagPluginInterface", "TriggerPluginInterface", "Plugin", "BagWriter", "BagReader", "TriggerRecorder",
    "utils", "helpers", "quantize", "QosProfile", "Status",
    "log_trace", "log_debug", "log_info", "log_warn", "log_error", "log_fatal",
    "TIMER_INFINITE", "VERSION", "VERSION_MAJOR", "VERSION_MINOR", "VERSION_PATCH",
    "__backend__",
]
if _HAS_MEMORY_RESOURCE:
    __all__.insert(__all__.index("MemoryPool") + 1, "MemoryResource")

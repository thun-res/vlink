#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD_DIR:-build-test}"
ctest_exclude="^(xmltest|ucunit)$|^CUnit_"
ctest_parallel="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

export VLINK_DDS_IP=127.0.0.1

case "$(uname -s)" in
    Darwin)
        export DYLD_LIBRARY_PATH="$PWD/$build_dir/output/bin:$PWD/$build_dir/output/lib:$PWD/$build_dir/output/external/lib:${DYLD_LIBRARY_PATH:-}"
        ;;
    *)
        export LD_LIBRARY_PATH="$PWD/$build_dir/output/bin:$PWD/$build_dir/output/lib:$PWD/$build_dir/output/external/lib:${LD_LIBRARY_PATH:-}"
        ;;
esac

proxy="$build_dir/output/bin/vlink-proxy"
if [ ! -x "$proxy" ]; then
    echo "Missing vlink-proxy before unit tests: $proxy" >&2
    exit 1
fi

proxy_pid=
proxy_log=

function stop_proxy() {
    if [ -z "$proxy_pid" ]; then
        return
    fi

    if kill -0 "$proxy_pid" 2>/dev/null; then
        kill -INT "$proxy_pid" 2>/dev/null || true
        for _ in $(seq 1 40); do
            if ! kill -0 "$proxy_pid" 2>/dev/null; then
                break
            fi
            local state
            state="$(ps -p "$proxy_pid" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
            if [ -z "$state" ] || [ "${state#Z}" != "$state" ]; then
                break
            fi
            sleep 0.25
        done
    fi

    local state
    state="$(ps -p "$proxy_pid" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
    if kill -0 "$proxy_pid" 2>/dev/null && [ "${state#Z}" = "$state" ]; then
        kill -TERM "$proxy_pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            if ! kill -0 "$proxy_pid" 2>/dev/null; then
                break
            fi
            state="$(ps -p "$proxy_pid" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
            if [ -z "$state" ] || [ "${state#Z}" != "$state" ]; then
                break
            fi
            sleep 0.25
        done
    fi

    state="$(ps -p "$proxy_pid" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
    if kill -0 "$proxy_pid" 2>/dev/null && [ "${state#Z}" = "$state" ]; then
        echo "vlink-proxy did not exit gracefully; forcing shutdown" >&2
        if [ -n "$proxy_log" ] && [ -f "$proxy_log" ]; then
            tail -100 "$proxy_log" || true
        fi
        kill -KILL "$proxy_pid" 2>/dev/null || true
    fi

    wait "$proxy_pid" 2>/dev/null || true
    proxy_pid=
    proxy_log=
}

function start_proxy() {
    stop_proxy
    proxy_log="$build_dir/vlink-proxy.log"
    "$proxy" -c -n -m off > "$proxy_log" 2>&1 &
    proxy_pid=$!
    sleep 2

    if ! kill -0 "$proxy_pid" 2>/dev/null; then
        tail -100 "$proxy_log" || true
        echo "vlink-proxy exited before unit tests" >&2
        exit 1
    fi
}

function run_ctest() {
    local parallel="${CTEST_PARALLEL_CURRENT:-$ctest_parallel}"
    ctest --test-dir "$build_dir" --output-on-failure --timeout 180 --parallel "$parallel" "$@"
}

trap stop_proxy EXIT

start_proxy
run_ctest --exclude-regex "$ctest_exclude"

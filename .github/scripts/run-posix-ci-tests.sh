#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD_DIR:-build-test}"
python_cmd="${PYTHON_CMD:-python}"
run_python_tests="${RUN_PYTHON_TESTS:-1}"
split_shm_tests="${SPLIT_SHM_TESTS:-1}"
ctest_exclude_base="${CTEST_EXCLUDE_BASE:-^(xmltest|ucunit)$|^CUnit_}"

case "$(uname -s)" in
  Darwin)
    export DYLD_LIBRARY_PATH="${PWD}/${build_dir}/output/bin:${PWD}/${build_dir}/output/lib:${PWD}/${build_dir}/output/external/lib:${DYLD_LIBRARY_PATH:-}"
    ;;
  *)
    export LD_LIBRARY_PATH="${PWD}/${build_dir}/output/bin:${PWD}/${build_dir}/output/lib:${PWD}/${build_dir}/output/external/lib:${LD_LIBRARY_PATH:-}"
    ;;
esac

proxy="${build_dir}/output/bin/vlink-proxy"
if [ ! -x "${proxy}" ]; then
  echo "Missing vlink-proxy before unit tests: ${proxy}" >&2
  exit 1
fi

proxy_pid=""
proxy_log=""

stop_proxy() {
  if [ -z "${proxy_pid}" ]; then
    return
  fi

  if kill -0 "${proxy_pid}" 2>/dev/null; then
    kill -INT "${proxy_pid}" 2>/dev/null || true
    for _ in $(seq 1 40); do
      if ! kill -0 "${proxy_pid}" 2>/dev/null; then
        break
      fi
      local state
      state="$(ps -p "${proxy_pid}" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
      if [ -z "${state}" ] || [ "${state#Z}" != "${state}" ]; then
        break
      fi
      sleep 0.25
    done
  fi

  local state
  state="$(ps -p "${proxy_pid}" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
  if kill -0 "${proxy_pid}" 2>/dev/null && [ "${state#Z}" = "${state}" ]; then
    kill -TERM "${proxy_pid}" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "${proxy_pid}" 2>/dev/null; then
        break
      fi
      state="$(ps -p "${proxy_pid}" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
      if [ -z "${state}" ] || [ "${state#Z}" != "${state}" ]; then
        break
      fi
      sleep 0.25
    done
  fi

  state="$(ps -p "${proxy_pid}" -o stat= 2>/dev/null | tr -d '[:space:]' || true)"
  if kill -0 "${proxy_pid}" 2>/dev/null && [ "${state#Z}" = "${state}" ]; then
    echo "vlink-proxy did not exit gracefully; forcing shutdown" >&2
    if [ -n "${proxy_log}" ] && [ -f "${proxy_log}" ]; then
      tail -100 "${proxy_log}" || true
    fi
    kill -KILL "${proxy_pid}" 2>/dev/null || true
  fi

  wait "${proxy_pid}" 2>/dev/null || true
  proxy_pid=""
  proxy_log=""
}

start_proxy() {
  stop_proxy
  local log_name="$1"
  proxy_log="${build_dir}/${log_name}.log"
  "${proxy}" -c -n -m off > "${proxy_log}" 2>&1 &
  proxy_pid="$!"
  sleep 2

  if ! kill -0 "${proxy_pid}" 2>/dev/null; then
    cat "${proxy_log}" || true
    echo "vlink-proxy exited before unit tests" >&2
    exit 1
  fi
}

run_ctest() {
  ctest --test-dir "${build_dir}" --output-on-failure --timeout 180 --parallel 1 "$@"
}

run_python() {
  if [ "${run_python_tests}" != "1" ]; then
    return
  fi

  export PYTHONPATH="${PWD}/python_api:${PWD}/${build_dir}/output/lib"
  if [ -n "${PYTHON_ASAN_OPTIONS:-}" ]; then
    ASAN_OPTIONS="${PYTHON_ASAN_OPTIONS}" "${python_cmd}" python_api/test/test_vlink.py
  else
    "${python_cmd}" python_api/test/test_vlink.py
  fi
}

trap stop_proxy EXIT

if [ "${split_shm_tests}" = "1" ]; then
  start_proxy "vlink-proxy-main"
  run_ctest --exclude-regex "${ctest_exclude_base}|^shm2?-"

  shm_suites=()
  while IFS= read -r suite; do
    shm_suites+=("${suite}")
  done < <(ctest --test-dir "${build_dir}" -N \
    | sed -n 's/^ *Test #[0-9][0-9]*: //p' \
    | grep -E '^shm2?-' || true)

  for suite in "${shm_suites[@]}"; do
    run_ctest --tests-regex "^${suite}$"
  done

  run_python
  stop_proxy
else
  start_proxy "vlink-proxy-main"
  run_ctest --exclude-regex "${ctest_exclude_base}"
  run_python
  stop_proxy
fi

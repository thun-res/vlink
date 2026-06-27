#!/usr/bin/env bash

vlink_progress_title=""
vlink_progress_steps=()
vlink_progress_status=()
vlink_progress_group_open=0

vlink_progress_summary_available() {
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] && [ -d "$(dirname "${GITHUB_STEP_SUMMARY}")" ] &&
    [ -w "$(dirname "${GITHUB_STEP_SUMMARY}")" ]
}

vlink_progress_escape_markdown() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//|/\\|}"
  printf '%s' "${value}"
}

vlink_progress_bar() {
  local done="$1"
  local total="$2"
  local width=20
  local filled=0
  local percent=0
  local i

  if [ "${total}" -gt 0 ]; then
    filled=$((done * width / total))
    percent=$((done * 100 / total))
  fi

  printf '['
  for ((i = 0; i < width; i++)); do
    if [ "${i}" -lt "${filled}" ]; then
      printf '#'
    else
      printf '-'
    fi
  done
  printf '] %d%%' "${percent}"
}

vlink_progress_done_count() {
  local count=0
  local status
  for status in "${vlink_progress_status[@]}"; do
    case "${status}" in
      Done|Skipped)
        count=$((count + 1))
        ;;
    esac
  done
  printf '%d' "${count}"
}

vlink_progress_log() {
  local current="$1"
  local state="$2"
  local total="${#vlink_progress_steps[@]}"
  local done
  done="$(vlink_progress_done_count)"
  printf 'vlink-progress %s %s/%s %s: %s\n' \
    "$(vlink_progress_bar "${done}" "${total}")" \
    "${done}" \
    "${total}" \
    "${state}" \
    "${current}"
}

vlink_progress_render() {
  vlink_progress_summary_available || return 0

  local current="${1:-}"
  local total="${#vlink_progress_steps[@]}"
  local done
  local i
  done="$(vlink_progress_done_count)"

  {
    printf '### %s\n\n' "$(vlink_progress_escape_markdown "${vlink_progress_title}")"
    printf '| Progress | Current |\n'
    printf '| --- | --- |\n'
    printf '| `%s` | %s |\n\n' "$(vlink_progress_bar "${done}" "${total}")" \
      "$(vlink_progress_escape_markdown "${current:-Idle}")"
    printf '| Step | Status |\n'
    printf '| --- | --- |\n'
    for ((i = 0; i < total; i++)); do
      printf '| %s | %s |\n' \
        "$(vlink_progress_escape_markdown "${vlink_progress_steps[$i]}")" \
        "$(vlink_progress_escape_markdown "${vlink_progress_status[$i]}")"
    done
  } > "${GITHUB_STEP_SUMMARY}"
}

vlink_progress_step_index() {
  local label="$1"
  local i
  for ((i = 0; i < ${#vlink_progress_steps[@]}; i++)); do
    if [ "${vlink_progress_steps[$i]}" = "${label}" ]; then
      printf '%d' "${i}"
      return 0
    fi
  done
  return 1
}

vlink_progress_set_status() {
  local label="$1"
  local status="$2"
  local index
  index="$(vlink_progress_step_index "${label}")" || return 0
  vlink_progress_status[$index]="${status}"
}

vlink_progress_group_start() {
  local label="$1"
  if [ "${vlink_progress_group_open}" -eq 1 ]; then
    vlink_progress_group_end
  fi
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    printf '::group::%s\n' "${label}"
  else
    printf '==> %s\n' "${label}"
  fi
  vlink_progress_group_open=1
}

vlink_progress_group_end() {
  if [ "${vlink_progress_group_open}" -eq 1 ]; then
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
      printf '::endgroup::\n'
    fi
    vlink_progress_group_open=0
  fi
}

vlink_progress_init() {
  vlink_progress_title="$1"
  shift
  vlink_progress_steps=("$@")
  vlink_progress_status=()

  local step
  for step in "${vlink_progress_steps[@]}"; do
    vlink_progress_status+=("Pending")
  done

  vlink_progress_render "Pending"
}

vlink_progress_start() {
  local label="$1"
  vlink_progress_set_status "${label}" "Running"
  vlink_progress_render "${label}"
  vlink_progress_log "${label}" "running"
  vlink_progress_group_start "${label}"
}

vlink_progress_complete() {
  local label="$1"
  vlink_progress_group_end
  vlink_progress_set_status "${label}" "Done"
  vlink_progress_render "${label}"
  vlink_progress_log "${label}" "done"
}

vlink_progress_skip() {
  local label="$1"
  vlink_progress_group_end
  vlink_progress_set_status "${label}" "Skipped"
  vlink_progress_render "${label}"
  vlink_progress_log "${label}" "skipped"
}

vlink_progress_fail() {
  local label="$1"
  vlink_progress_group_end
  vlink_progress_set_status "${label}" "Failed"
  vlink_progress_render "${label}"
  vlink_progress_log "${label}" "failed"
}

vlink_progress_run() {
  local label="$1"
  shift

  vlink_progress_start "${label}"
  if "$@"; then
    vlink_progress_complete "${label}"
  else
    local status=$?
    vlink_progress_fail "${label}"
    return "${status}"
  fi
}

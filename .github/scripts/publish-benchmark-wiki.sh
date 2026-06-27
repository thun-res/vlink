#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Publish benchmark wiki" \
  "Validate inputs" \
  "Parse benchmark report" \
  "Clone wiki" \
  "Render Benchmarks page" \
  "Commit wiki changes" \
  "Push wiki changes"

vlink_progress_start "Validate inputs"
repo="${GITHUB_REPOSITORY:-}"
token="${GITHUB_TOKEN:-}"
report_url="${BENCH_PAGES_URL:-}"
json_file="${BENCH_JSON_FILE:-build-doc/output/doc/bench/index.json}"
if [ -z "${repo}" ] || [ -z "${token}" ] || [ -z "${report_url}" ]; then
  echo "GITHUB_REPOSITORY, GITHUB_TOKEN, and BENCH_PAGES_URL are required." >&2
  vlink_progress_fail "Validate inputs"
  exit 1
fi
report_url="${report_url%/}"

if [ ! -f "${json_file}" ]; then
  echo "Benchmark JSON not found: ${json_file}" >&2
  vlink_progress_fail "Validate inputs"
  exit 1
fi
vlink_progress_complete "Validate inputs"

vlink_progress_start "Parse benchmark report"
if parsed_report="$(
  jq -r '[
    .version // "unknown",
    .platform // "unknown",
    .host_name // "unknown",
    .created_at // "unknown",
    ((.scenarios // []) | length),
    ([(.scenarios // [])[] | select(.success)] | length),
    (.skipped_case_count // 0)
  ] | @tsv' "${json_file}"
)"; then
  IFS=$'\t' read -r version platform host_name created_at total_cases passing_cases skipped_cases <<< "${parsed_report}"
  vlink_progress_complete "Parse benchmark report"
else
  status=$?
  vlink_progress_fail "Parse benchmark report"
  exit "${status}"
fi

auth_header="$(printf 'x-access-token:%s' "${token}" | base64 | tr -d '\n')"
vlink_progress_start "Clone wiki"
if ! wiki_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vlink-bench-wiki.XXXXXX")"; then
  vlink_progress_fail "Clone wiki"
  exit 1
fi
trap 'rm -rf "${wiki_dir}"' EXIT
if git -c "http.https://github.com/.extraheader=AUTHORIZATION: basic ${auth_header}" \
  clone --quiet --depth 1 "https://github.com/${repo}.wiki.git" "${wiki_dir}"; then
  vlink_progress_complete "Clone wiki"
else
  status=$?
  vlink_progress_fail "Clone wiki"
  exit "${status}"
fi

vlink_progress_start "Render Benchmarks page"
if top_throughput_rows="$(
  jq -r '
    (.scenarios // [])
    | map(select(.success and ((.recv_msgs_per_sec // 0) > 0)))
    | sort_by(-(.recv_msgs_per_sec // 0))
    | .[:15][]
    | "| \(.transport) | \(.scenario.suite) | \(.scenario.payload_size) | "
      + "\((.recv_msgs_per_sec // 0) | floor) | "
      + "\(((.recv_mb_per_sec // 0) * 100 | round) / 100) | "
      + "\(((.p50_latency_us // 0) * 10 | round) / 10) |"
  ' "${json_file}"
)"; then
  :
else
  status=$?
  vlink_progress_fail "Render Benchmarks page"
  exit "${status}"
fi

page="${wiki_dir}/Benchmarks.md"
if {
  printf '# 🚀 性能基准\n\n'
  printf '> 数据来自最近一次发布的 `vlink-bench` 运行（`quick` 预设）\n\n'
  printf '🔗 完整交互式报告（图表、评分热力图、逐用例明细）——'
  printf '**[在 GitHub Pages 查看](%s/)**\n\n' "${report_url}"

  printf '## 🧾 运行概览\n\n'
  printf '| 字段 | 值 |\n'
  printf '| --- | --- |\n'
  printf '| 版本 | `%s` |\n' "${version}"
  printf '| 平台 | %s |\n' "${platform}"
  printf '| 主机 | %s |\n' "${host_name}"
  printf '| 生成时间 | %s |\n' "${created_at}"
  printf '| 通过 / 总用例 | %s / %s |\n' "${passing_cases}" "${total_cases}"
  printf '| 跳过 | %s |\n\n' "${skipped_cases}"

  printf '## 📈 吞吐 Top 结果\n\n'
  printf '| 传输后端 | 套件 | 负载 (B) | 接收 msg/s | 接收 MB/s | p50 时延 (µs) |\n'
  printf '| --- | --- | ---: | ---: | ---: | ---: |\n'
  printf '%s\n' "${top_throughput_rows}"
  printf '\n'

  printf '完整 HTML 报告发布于 GitHub Pages 并随发布版本附带；本页仅汇总核心指标。\n\n'
  printf -- '---\n\n'
  printf '_🕒 更新于 `%s`。_\n' "${GITHUB_SHA:-unknown}"
} > "${page}"; then
  vlink_progress_complete "Render Benchmarks page"
else
  status=$?
  vlink_progress_fail "Render Benchmarks page"
  exit "${status}"
fi

vlink_progress_start "Commit wiki changes"
if ! git -C "${wiki_dir}" config user.name "github-actions[bot]" ||
  ! git -C "${wiki_dir}" config user.email "41898282+github-actions[bot]@users.noreply.github.com" ||
  ! git -C "${wiki_dir}" add Benchmarks.md; then
  vlink_progress_fail "Commit wiki changes"
  exit 1
fi

set +e
git -C "${wiki_dir}" diff --cached --quiet
diff_status=$?
set -e
case "${diff_status}" in
  0)
    echo "No benchmark wiki changes."
    vlink_progress_skip "Commit wiki changes"
    vlink_progress_skip "Push wiki changes"
    exit 0
    ;;
  1)
    ;;
  *)
    vlink_progress_fail "Commit wiki changes"
    exit "${diff_status}"
    ;;
esac

if git -C "${wiki_dir}" commit -m "docs: update benchmark report"; then
  vlink_progress_complete "Commit wiki changes"
else
  status=$?
  vlink_progress_fail "Commit wiki changes"
  exit "${status}"
fi

vlink_progress_run "Push wiki changes" \
  git -C "${wiki_dir}" -c "http.https://github.com/.extraheader=AUTHORIZATION: basic ${auth_header}" \
  push origin HEAD:master

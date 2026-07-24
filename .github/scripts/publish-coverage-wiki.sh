#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

vlink_progress_init "Publish coverage wiki" \
    "Validate inputs" \
    "Parse coverage summary" \
    "Clone wiki" \
    "Render Coverage page" \
    "Commit wiki changes" \
    "Push wiki changes"

vlink_progress_start "Validate inputs"
repo="${GITHUB_REPOSITORY:-}"
token="${GITHUB_TOKEN:-}"
report_url="${COVERAGE_PAGES_URL:-}"
summary_file="${COVERAGE_SUMMARY_FILE:-build-doc/output/doc/coverage/coverage-summary.txt}"
if [ -z "$repo" ] || [ -z "$token" ] || [ -z "$report_url" ]; then
    echo "GITHUB_REPOSITORY, GITHUB_TOKEN, and COVERAGE_PAGES_URL are required." >&2
    vlink_progress_fail "Validate inputs"
    exit 1
fi
report_url="${report_url%/}"

if [ ! -f "$summary_file" ]; then
    echo "Coverage summary not found: $summary_file" >&2
    vlink_progress_fail "Validate inputs"
    exit 1
fi
vlink_progress_complete "Validate inputs"

# Pull the overall line/function/branch rates out of the coverage summary. The
# format differs between gcovr (`lines: 82.5% (1234 out of 1496)`) and
# lcov/genhtml (`  lines......: 82.5% (1234 of 1496 lines)`), so match both by
# looking for the metric keyword followed by a percentage and the covered/total
# counts.
function parse_metric() {
    local keyword="$1"

    grep -iE "^[[:space:]]*${keyword}[[:space:].]*:" "$summary_file" | tail -1 |
        sed -E "s/.*:[[:space:]]*([0-9.]+%)[[:space:]]*\\(([0-9]+)[^0-9]+([0-9]+).*/\\1 (\\2\\/\\3)/" |
        head -1
}

vlink_progress_start "Parse coverage summary"
lines_rate="$(parse_metric lines || true)"
functions_rate="$(parse_metric functions || true)"
branches_rate="$(parse_metric branches || true)"
vlink_progress_complete "Parse coverage summary"

auth_header="$(printf 'x-access-token:%s' "$token" | base64 | tr -d '\n')"
vlink_progress_start "Clone wiki"
if ! wiki_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vlink-coverage-wiki.XXXXXX")"; then
    vlink_progress_fail "Clone wiki"
    exit 1
fi
trap 'rm -rf "$wiki_dir"' EXIT
if git -c "http.https://github.com/.extraheader=AUTHORIZATION: basic $auth_header" \
    clone --quiet --depth 1 "https://github.com/$repo.wiki.git" "$wiki_dir"; then
    vlink_progress_complete "Clone wiki"
else
    status=$?
    vlink_progress_fail "Clone wiki"
    exit "$status"
fi

vlink_progress_start "Render Coverage page"
if raw_coverage_summary="$(tail -40 "$summary_file")"; then
    :
else
    status=$?
    vlink_progress_fail "Render Coverage page"
    exit "$status"
fi

page="$wiki_dir/Coverage.md"
if {
    printf '# 🧪 代码覆盖率\n\n'
    printf '> 行 / 函数 / 分支覆盖率，来自最近一次发布的 `CI Coverage` 运行\n\n'
    printf '🔗 完整 HTML 报告：**[在 GitHub Pages 查看](%s/)**\n\n' "$report_url"
    printf '| 指标 | 覆盖率（已覆盖 / 总数） |\n'
    printf '| --- | --- |\n'
    printf '| 行 | %s |\n' "${lines_rate:-n/a}"
    printf '| 函数 | %s |\n' "${functions_rate:-n/a}"
    printf '| 分支 | %s |\n' "${branches_rate:-n/a}"
    printf '\n'
    printf '<details>\n<summary>原始覆盖率摘要</summary>\n\n```\n'
    printf '%s\n' "$raw_coverage_summary"
    printf '\n```\n\n</details>\n\n'
    printf -- '---\n\n'
    printf '_🕒 更新于 `%s`。_\n' "${GITHUB_SHA:-unknown}"
} > "$page"; then
    vlink_progress_complete "Render Coverage page"
else
    status=$?
    vlink_progress_fail "Render Coverage page"
    exit "$status"
fi

vlink_progress_start "Commit wiki changes"
if ! git -C "$wiki_dir" config user.name "github-actions[bot]" ||
    ! git -C "$wiki_dir" config user.email "41898282+github-actions[bot]@users.noreply.github.com" ||
    ! git -C "$wiki_dir" add Coverage.md; then
    vlink_progress_fail "Commit wiki changes"
    exit 1
fi

if git -C "$wiki_dir" diff --cached --quiet; then
    diff_status=0
else
    diff_status=$?
fi
case "$diff_status" in
    0)
        echo "No coverage wiki changes."
        vlink_progress_skip "Commit wiki changes"
        vlink_progress_skip "Push wiki changes"
        exit 0
        ;;
    1)
        ;;
    *)
        vlink_progress_fail "Commit wiki changes"
        exit "$diff_status"
        ;;
esac

if git -C "$wiki_dir" commit -m "docs: update code coverage report"; then
    vlink_progress_complete "Commit wiki changes"
else
    status=$?
    vlink_progress_fail "Commit wiki changes"
    exit "$status"
fi

vlink_progress_run "Push wiki changes" \
    git -C "$wiki_dir" -c "http.https://github.com/.extraheader=AUTHORIZATION: basic $auth_header" \
        push origin HEAD:master

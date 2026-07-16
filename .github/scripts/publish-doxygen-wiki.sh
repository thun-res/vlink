#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Publish Doxygen wiki links" \
  "Validate inputs" \
  "Clone wiki" \
  "Render Home page" \
  "Commit wiki changes" \
  "Push wiki changes"

vlink_progress_start "Validate inputs"
repo="${GITHUB_REPOSITORY:-}"
token="${GITHUB_TOKEN:-}"
pages_url="${DOXYGEN_PAGES_URL:-}"
if [ -z "${repo}" ] || [ -z "${token}" ] || [ -z "${pages_url}" ]; then
  echo "GITHUB_REPOSITORY, GITHUB_TOKEN, and DOXYGEN_PAGES_URL are required." >&2
  vlink_progress_fail "Validate inputs"
  exit 1
fi
pages_url="${pages_url%/}"
vlink_progress_complete "Validate inputs"

auth_header="$(printf 'x-access-token:%s' "${token}" | base64 | tr -d '\n')"
vlink_progress_start "Clone wiki"
if ! wiki_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vlink-wiki.XXXXXX")"; then
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

vlink_progress_start "Render Home page"
home="${wiki_dir}/Home.md"
if {
  printf '# ⚡ VLink\n\n'
  printf '> 面向自动驾驶与具身智能的高性能通信中间件\n\n'
  printf '一套类型安全的统一 API 覆盖进程内、共享内存、车载以太网与跨机网络，'
  printf '更换通信后端仅需修改 URL 前缀，业务代码无需改动。\n\n'
  printf '当前支持 **12 种传输后端**、**14 种序列化格式**、**3 种通信模型**与 '
  printf '**6 个核心原语**，并提供安全加密、录制回放、服务发现、命令行工具及 '
  printf 'Foxglove / Rerun 可视化桥接。\n\n'

  printf '## 📚 文档\n\n'
  printf '| 资源 | 链接 |\n'
  printf '| --- | --- |\n'
  printf -- '| 🌐 官方网站 | [%s/](%s/) |\n' "${pages_url}" "${pages_url}"
  printf -- '| 📖 开发者文档（中文） | [%s/zh_cn/](%s/zh_cn/) |\n' "${pages_url}" "${pages_url}"
  printf -- '| 📘 开发者文档（英文） | [%s/en_us/](%s/en_us/) |\n\n' "${pages_url}" "${pages_url}"

  printf '## 📊 项目报告\n\n'
  printf '| 报告 | 摘要 | 完整报告 |\n'
  printf '| --- | --- | --- |\n'
  printf -- '| 🚀 性能基准 | [Benchmarks](Benchmarks) | [%s/bench/](%s/bench/) |\n' "${pages_url}" "${pages_url}"
  printf -- '| 🧪 代码覆盖率 | [Coverage](Coverage) | [%s/coverage/](%s/coverage/) |\n\n' "${pages_url}" "${pages_url}"

  printf '## 🧭 资源\n\n'
  printf -- '- 📦 源码仓库 — https://github.com/%s\n' "${repo}"
  printf -- '- 🛠️ 构建工具 vkit — https://github.com/thun-res/vkit\n'
  printf -- '- ⬇️ 发布下载 — https://github.com/%s/releases\n' "${repo}"
  printf -- '- 💬 问题反馈 — https://github.com/%s/issues\n\n' "${repo}"

  printf -- '---\n\n'
  printf '_🕒 更新于 `%s`。_\n' "${GITHUB_SHA:-unknown}"
} > "${home}"; then
  vlink_progress_complete "Render Home page"
else
  status=$?
  vlink_progress_fail "Render Home page"
  exit "${status}"
fi

vlink_progress_start "Commit wiki changes"
if ! git -C "${wiki_dir}" config user.name "github-actions[bot]" ||
  ! git -C "${wiki_dir}" config user.email "41898282+github-actions[bot]@users.noreply.github.com" ||
  ! git -C "${wiki_dir}" add Home.md; then
  vlink_progress_fail "Commit wiki changes"
  exit 1
fi

set +e
git -C "${wiki_dir}" diff --cached --quiet
diff_status=$?
set -e
case "${diff_status}" in
  0)
    echo "No Doxygen wiki changes."
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

if git -C "${wiki_dir}" commit -m "docs: update Doxygen wiki links"; then
  vlink_progress_complete "Commit wiki changes"
else
  status=$?
  vlink_progress_fail "Commit wiki changes"
  exit "${status}"
fi

vlink_progress_run "Push wiki changes" \
  git -C "${wiki_dir}" -c "http.https://github.com/.extraheader=AUTHORIZATION: basic ${auth_header}" \
  push origin HEAD:master

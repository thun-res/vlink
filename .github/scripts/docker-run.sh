#!/usr/bin/env bash
set -euo pipefail

image="${1:?Docker image is required}"
shift

use_cache="${VLINK_DOCKER_RUN_CACHE:-1}"
if [ "${use_cache}" = "1" ]; then
  docker_home="${HOME}/.vlink-docker-home"
else
  docker_home="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vlink-docker-home.XXXXXX")"
fi

cleanup() {
  if [ "${use_cache}" != "1" ]; then
    rm -rf "${docker_home}"
  fi
}
trap cleanup EXIT

mkdir -p "${docker_home}"

docker_args=(
  --rm
  --user "$(id -u):$(id -g)"
  --shm-size=2g
  --volume /etc/passwd:/etc/passwd:ro
  --volume /etc/group:/etc/group:ro
  --volume "${GITHUB_WORKSPACE}:/work/vlink"
  --volume "${docker_home}:/github/home"
  --workdir /work/vlink
  --env HOME=/github/home
  --env GITHUB_WORKSPACE=/work/vlink
  --env LANG=C.UTF-8
  --env LC_ALL=C.UTF-8
  --env CMAKE_BUILD_PARALLEL_LEVEL
  --env CMAKE_GENERATOR
  --env PIP_DISABLE_PIP_VERSION_CHECK
  --env VLINK_CI_CMAKE_ARGS
  --env VLINK_CI_EXTRA_CMAKE_ARGS
)

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  github_step_summary_dir="$(dirname "${GITHUB_STEP_SUMMARY}")"
  if [ -d "${github_step_summary_dir}" ]; then
    docker_args+=(
      --volume "${github_step_summary_dir}:${github_step_summary_dir}"
      --env GITHUB_STEP_SUMMARY
    )
  fi
fi

if [ -n "${GITHUB_ACTIONS:-}" ]; then
  docker_args+=(--env GITHUB_ACTIONS)
fi

if [ "${use_cache}" != "0" ]; then
  mkdir -p \
    "${HOME}/.conan2" \
    "${HOME}/.vlink-cpm-cache"
  docker_args+=(
    --volume "${HOME}/.conan2:/github/home/.conan2"
    --volume "${HOME}/.vlink-cpm-cache:/github/home/.vlink-cpm-cache"
  )
fi

if [ "${use_cache}" = "1" ]; then
  mkdir -p \
    "${HOME}/.ccache"
  docker_args+=(
    --volume "${HOME}/.ccache:/github/home/.ccache"
    --env CCACHE_DIR=/github/home/.ccache
    --env CCACHE_BASEDIR=/work/vlink
    --env CCACHE_COMPRESS=true
    --env CCACHE_COMPILERCHECK=content
    --env CCACHE_MAXSIZE=2G
  )
fi

docker run "${docker_args[@]}" "${image}" \
  bash -c 'git config --global --add safe.directory "${GITHUB_WORKSPACE}" && exec "$@"' bash "$@"

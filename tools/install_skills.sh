#!/usr/bin/env bash

SHELL_DIR=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
ROOT_DIR=$(cd "$SHELL_DIR/.." && pwd)
SRC_DIR="$ROOT_DIR/.agents/skills"
AGENTS_FILE="$ROOT_DIR/AGENTS.md"
CLAUDE_FILE="$ROOT_DIR/CLAUDE.md"
COPY_MODE=0
COPY_MARKER=".vlink-skills-copy"
COPY_VERSION="v1"
OPTION=
[ "$#" -gt 0 ] && OPTION="$1"

[ "$#" -gt 1 ] && echo -e "\nError: expected at most one option\n" >&2 && exit 1
([ "$OPTION" = "-h" ] || [ "$OPTION" = "--help" ]) &&
    echo -e "Usage:\n  install_skills.sh [--copy]\n\nInstall CLAUDE.md and .agents/skills for Claude and Codex." &&
    exit 0

[ -n "$OPTION" ] && [ "$OPTION" != "--copy" ] &&
    echo -e "\nError: unknown option '$OPTION'\n" >&2 && exit 1
[ "$OPTION" = "--copy" ] && COPY_MODE=1

function _resolve_mode() {
    local _dst_dir="$1"

    TARGET_MODE=link
    [ "$COPY_MODE" -eq 1 ] && TARGET_MODE=copy
    if [ ! -e "$_dst_dir" ] && [ ! -L "$_dst_dir" ]; then
        return 0
    fi
    if [ -L "$_dst_dir" ] && [ "$(readlink "$_dst_dir")" = "../.agents/skills" ]; then
        return 0
    fi
    if [ -d "$_dst_dir" ] &&
        [ -f "$_dst_dir/$COPY_MARKER" ] &&
        [ "$(cat "$_dst_dir/$COPY_MARKER")" = "$COPY_VERSION" ]; then
        [ "$COPY_MODE" -eq 0 ] && TARGET_MODE=copy
        return 0
    fi

    echo -e "\nError: $_dst_dir already exists and is not managed by this script\n" >&2
    return 1
}

function _install_target() {
    local _agent_dir="$1"
    local _mode="$2"
    local _parent_dir="$ROOT_DIR/$_agent_dir"
    local _dst_dir="$_parent_dir/skills"

    mkdir -p "$_parent_dir" || return 1
    if [ -e "$_dst_dir" ] || [ -L "$_dst_dir" ]; then
        rm -rf "$_dst_dir" || return 1
    fi

    if [ "$_mode" = copy ]; then
        mkdir -p "$_dst_dir" || return 1
        printf '%s\n' "$COPY_VERSION" > "$_dst_dir/$COPY_MARKER" || return 1
        cp -R "$SRC_DIR/." "$_dst_dir/" || return 1
        echo "Copied: ${_dst_dir#"$ROOT_DIR/"}"
    else
        ln -s "../.agents/skills" "$_dst_dir" || return 1
        if [ ! -L "$_dst_dir" ] || [ "$(readlink "$_dst_dir")" != "../.agents/skills" ]; then
            rm -rf "$_dst_dir"
            echo -e "\nError: failed to create a symbolic link: $_dst_dir\nUse --copy for skills on this platform.\n" >&2
            return 1
        fi
        echo "Linked: ${_dst_dir#"$ROOT_DIR/"} -> ../.agents/skills"
    fi
}

[ -f "$AGENTS_FILE" ] || {
    echo -e "\nError: file not found: $AGENTS_FILE\n" >&2
    exit 1
}
[ -d "$SRC_DIR" ] || {
    echo -e "\nError: directory not found: $SRC_DIR\n" >&2
    exit 1
}

SKILL_COUNT=0
for _skill_dir in "$SRC_DIR"/*; do
    [ -d "$_skill_dir" ] || continue
    [ -f "$_skill_dir/SKILL.md" ] || {
        echo -e "\nError: file not found: $_skill_dir/SKILL.md\n" >&2
        exit 1
    }
    SKILL_COUNT=$((SKILL_COUNT + 1))
done
[ "$SKILL_COUNT" -gt 0 ] || {
    echo -e "\nError: no skills found in $SRC_DIR\n" >&2
    exit 1
}

_resolve_mode "$ROOT_DIR/.claude/skills" || exit 1
CLAUDE_MODE="$TARGET_MODE"
_resolve_mode "$ROOT_DIR/.codex/skills" || exit 1
CODEX_MODE="$TARGET_MODE"

if [ -e "$CLAUDE_FILE" ] || [ -L "$CLAUDE_FILE" ]; then
    if [ ! -L "$CLAUDE_FILE" ] || [ "$(readlink "$CLAUDE_FILE")" != "AGENTS.md" ]; then
        echo -e "\nError: $CLAUDE_FILE already exists and is not managed by this script\n" >&2
        exit 1
    fi
fi

_install_target ".claude" "$CLAUDE_MODE" || exit 1
_install_target ".codex" "$CODEX_MODE" || exit 1

rm -f "$CLAUDE_FILE" || exit 1
ln -s "AGENTS.md" "$CLAUDE_FILE" || exit 1
if [ ! -L "$CLAUDE_FILE" ] || [ "$(readlink "$CLAUDE_FILE")" != "AGENTS.md" ]; then
    rm -f "$CLAUDE_FILE"
    echo -e "\nError: failed to create the CLAUDE.md symbolic link\nEnable native symbolic links and rerun this script.\n" >&2
    exit 1
fi
echo "Linked: CLAUDE.md -> AGENTS.md"

echo -e "\nInstalled $SKILL_COUNT skills:\n"
for _skill_dir in "$SRC_DIR"/*; do
    [ -d "$_skill_dir" ] || continue
    echo "  /$(basename "$_skill_dir")"
done

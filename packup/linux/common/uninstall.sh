#!/usr/bin/env bash

VLINK_ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
VLINK_BIN_DIR="$VLINK_ROOT_DIR/bin"
DESKTOP_DIR="$VLINK_ROOT_DIR/desktop"

echo "Uninstall..."

rm -f "$HOME/.local/share/icons/vlink-viewer.png"              2>/dev/null
rm -f "$HOME/.local/share/icons/vlink-player.png"              2>/dev/null
rm -f "$HOME/.local/share/icons/vlink-analyzer.png"            2>/dev/null
rm -f "$HOME/.local/share/icons/vlink-cmd.png"                 2>/dev/null
rm -f "$HOME/.local/share/applications/vlink-viewer.desktop"   2>/dev/null
rm -f "$HOME/.local/share/applications/vlink-player.desktop"   2>/dev/null
rm -f "$HOME/.local/share/applications/vlink-analyzer.desktop" 2>/dev/null
rm -f "$HOME/.local/share/applications/vlink-cmd.desktop"      2>/dev/null

if command -v xdg-user-dir &>/dev/null; then
    DESKTOP="$(xdg-user-dir DESKTOP 2>/dev/null)"
    if [ -n "$DESKTOP" ] && [ -d "$DESKTOP" ] && [ "$DESKTOP" != "$HOME" ]; then
        for _f in vlink-viewer vlink-player vlink-analyzer vlink-cmd; do
            if [ -f "$DESKTOP/${_f}.desktop" ] || [ -L "$DESKTOP/${_f}.desktop" ]; then
                gio remove "$DESKTOP/${_f}.desktop" &>/dev/null
                rm -f "$DESKTOP/${_f}.desktop" 2>/dev/null
            fi
        done
    fi
fi

if [ -f "$DESKTOP_DIR/x-vlink-bag.xml" ]; then
    xdg-mime uninstall --mode user "$DESKTOP_DIR/x-vlink-bag.xml" &>/dev/null
fi
rm -f "$HOME/.local/share/mime/packages/x-vlink-bag.xml" 2>/dev/null

for _f in "$HOME/.config/mimeapps.list" "$HOME/.local/share/applications/mimeapps.list"; do
    if [ -f "$_f" ]; then
        sed -i.vlink_bak -E \
            -e '/^application\/(x-)?vlink-(vdb|vdbx|vcap|vcapx)[[:space:]]*=/{
                s/vlink-player\.desktop;//g
                s/vlink-player\.desktop//g
            }' \
            -e '/^application\/(x-)?vlink-(vdb|vdbx|vcap|vcapx)[[:space:]]*=[[:space:]]*$/d' \
            "$_f" 2>/dev/null
        rm -f "${_f}.vlink_bak" 2>/dev/null
    fi
done

update-desktop-database "$HOME/.local/share/applications/" &>/dev/null
update-mime-database    "$HOME/.local/share/mime/"         &>/dev/null

uninstall_theme_icon() {
    local _theme_dir="$1"
    [ -d "$_theme_dir" ] || return 0
    local _touched=0
    for _path in \
        "$_theme_dir/256x256/mimetypes/vlink-vdb.png" \
        "$_theme_dir/256x256/mimetypes/vlink-vdbx.png" \
        "$_theme_dir/256x256/mimetypes/vlink-vcap.png" \
        "$_theme_dir/256x256/mimetypes/vlink-vcapx.png" \
        "$_theme_dir/scalable/mimetypes/vlink-vdb.svg" \
        "$_theme_dir/scalable/mimetypes/vlink-vdbx.svg" \
        "$_theme_dir/scalable/mimetypes/vlink-vcap.svg" \
        "$_theme_dir/scalable/mimetypes/vlink-vcapx.svg"; do
        if [ -e "$_path" ]; then
            rm -f "$_path" 2>/dev/null
            _touched=1
        fi
    done
    if [ "$_touched" = "1" ] && command -v gtk-update-icon-cache &>/dev/null; then
        gtk-update-icon-cache "$_theme_dir/" &>/dev/null
    fi
}

if [ -d "$HOME/.local/share/icons" ]; then
    shopt -s nullglob
    for _theme_dir in "$HOME/.local/share/icons"/*/; do
        uninstall_theme_icon "${_theme_dir%/}"
    done
    shopt -u nullglob
fi

rm -f "$HOME/.config/vlink/install_path" 2>/dev/null
rmdir "$HOME/.config/vlink" 2>/dev/null

echo "Done."
exit 0

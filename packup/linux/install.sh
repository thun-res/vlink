#!/usr/bin/env bash

VLINK_ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
VLINK_BIN_DIR="$VLINK_ROOT_DIR/bin"
DESKTOP_DIR="$VLINK_ROOT_DIR/desktop"

echo "Install..."

mkdir -p "$HOME/.local/share/applications/"
mkdir -p "$HOME/.local/share/icons/"

cp -f "$DESKTOP_DIR/vlink-viewer.png"        "$HOME/.local/share/icons/"
cp -f "$DESKTOP_DIR/vlink-viewer.desktop"    "$HOME/.local/share/applications/"
cp -f "$DESKTOP_DIR/vlink-player.png"        "$HOME/.local/share/icons/"
cp -f "$DESKTOP_DIR/vlink-player.desktop"    "$HOME/.local/share/applications/"
cp -f "$DESKTOP_DIR/vlink-analyzer.png"      "$HOME/.local/share/icons/"
cp -f "$DESKTOP_DIR/vlink-analyzer.desktop"  "$HOME/.local/share/applications/"
cp -f "$DESKTOP_DIR/vlink-cmd.png"           "$HOME/.local/share/icons/"
cp -f "$DESKTOP_DIR/vlink-cmd.desktop"       "$HOME/.local/share/applications/"

_sed_escape() {
    printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'
}
_DESKTOP_ESC="$(_sed_escape "$DESKTOP_DIR")"
_BIN_ESC="$(_sed_escape "$VLINK_BIN_DIR")"

_rewrite_desktop() {
    local _f="$1"; local _base="$2"
    sed -i \
        -e "s|Icon=${_base}\.png|Icon=${_DESKTOP_ESC}/${_base}.png|g" \
        -e "s|Exec=run_${_base#vlink-}\.sh|Exec=\"${_BIN_ESC}/run_${_base#vlink-}.sh\"|g" \
        "$_f"
}
_rewrite_desktop "$HOME/.local/share/applications/vlink-viewer.desktop"   vlink-viewer
_rewrite_desktop "$HOME/.local/share/applications/vlink-player.desktop"   vlink-player
_rewrite_desktop "$HOME/.local/share/applications/vlink-analyzer.desktop" vlink-analyzer
_rewrite_desktop "$HOME/.local/share/applications/vlink-cmd.desktop"      vlink-cmd

chmod a+x "$HOME/.local/share/applications/vlink-viewer.desktop"   2>/dev/null
chmod a+x "$HOME/.local/share/applications/vlink-player.desktop"   2>/dev/null
chmod a+x "$HOME/.local/share/applications/vlink-analyzer.desktop" 2>/dev/null
chmod a+x "$HOME/.local/share/applications/vlink-cmd.desktop"      2>/dev/null

if command -v xdg-user-dir &>/dev/null; then
    DESKTOP="$(xdg-user-dir DESKTOP 2>/dev/null)"
    if [ -n "$DESKTOP" ] && [ -d "$DESKTOP" ] && [ "$DESKTOP" != "$HOME" ]; then
        cp -f "$HOME/.local/share/applications/vlink-viewer.desktop"   "$DESKTOP/"
        cp -f "$HOME/.local/share/applications/vlink-player.desktop"   "$DESKTOP/"
        cp -f "$HOME/.local/share/applications/vlink-analyzer.desktop" "$DESKTOP/"
        cp -f "$HOME/.local/share/applications/vlink-cmd.desktop"      "$DESKTOP/"
        gio set "$DESKTOP/vlink-viewer.desktop"   metadata::trusted true &>/dev/null
        gio set "$DESKTOP/vlink-player.desktop"   metadata::trusted true &>/dev/null
        gio set "$DESKTOP/vlink-analyzer.desktop" metadata::trusted true &>/dev/null
        gio set "$DESKTOP/vlink-cmd.desktop"      metadata::trusted true &>/dev/null
    fi
fi

if grep -q "Ubuntu" /etc/os-release 2>/dev/null || grep -q "Ubuntu" /etc/issue 2>/dev/null; then
    _missing=()
    for _pkg in libvdpau-dev libva-drm2 libva-x11-2 libxcb-cursor0; do
        dpkg-query -W -f='${Status}' "$_pkg" 2>/dev/null | grep -q "install ok installed" \
            || _missing+=("$_pkg")
    done
    if [ ! -e "$VLINK_ROOT_DIR/lib/platforms/libqxcb.so" ]; then
        for _pkg in libqt5core5a libqt5gui5 libqt5widgets5 libqt5dbus5 libqt5sql5 libqt5sql5-sqlite libqt5network5 libqt5opengl5 libqt5svg5 qt5-qpa-plugins; do
            dpkg-query -W -f='${Status}' "$_pkg" 2>/dev/null | grep -q "install ok installed" \
                || _missing+=("$_pkg")
        done
    fi

    if [ ${#_missing[@]} -gt 0 ]; then
        echo "The following packages are required: ${_missing[*]}"
        if command -v sudo &>/dev/null && sudo -n true 2>/dev/null; then
            sudo apt-get install -y "${_missing[@]}" || true
        elif [ -t 0 ] && command -v sudo &>/dev/null; then
            sudo apt-get install -y "${_missing[@]}" || true
        else
            echo "No interactive elevation available. Please install manually:"
            echo "  sudo apt-get install -y ${_missing[*]}"
        fi
    fi
fi

xdg-mime install --mode user "$DESKTOP_DIR/x-vlink-bag.xml" &>/dev/null

xdg-mime default vlink-player.desktop application/vlink-vdb   2>/dev/null
xdg-mime default vlink-player.desktop application/vlink-vdbx  2>/dev/null
xdg-mime default vlink-player.desktop application/vlink-vcap  2>/dev/null
xdg-mime default vlink-player.desktop application/vlink-vcapx 2>/dev/null
update-desktop-database "$HOME/.local/share/applications/" &>/dev/null
update-mime-database    "$HOME/.local/share/mime/"         &>/dev/null

install_theme_icon() {
    local _theme_name="$1"
    mkdir -p "$HOME/.local/share/icons/$_theme_name/256x256/mimetypes/"
    mkdir -p "$HOME/.local/share/icons/$_theme_name/scalable/mimetypes/"
    cp -f "$DESKTOP_DIR/vlink-player.png" "$HOME/.local/share/icons/$_theme_name/256x256/mimetypes/vlink-vdb.png"   2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.png" "$HOME/.local/share/icons/$_theme_name/256x256/mimetypes/vlink-vdbx.png"  2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.png" "$HOME/.local/share/icons/$_theme_name/256x256/mimetypes/vlink-vcap.png"  2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.png" "$HOME/.local/share/icons/$_theme_name/256x256/mimetypes/vlink-vcapx.png" 2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.svg" "$HOME/.local/share/icons/$_theme_name/scalable/mimetypes/vlink-vdb.svg"   2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.svg" "$HOME/.local/share/icons/$_theme_name/scalable/mimetypes/vlink-vdbx.svg"  2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.svg" "$HOME/.local/share/icons/$_theme_name/scalable/mimetypes/vlink-vcap.svg"  2>/dev/null
    cp -f "$DESKTOP_DIR/vlink-player.svg" "$HOME/.local/share/icons/$_theme_name/scalable/mimetypes/vlink-vcapx.svg" 2>/dev/null
    if command -v gtk-update-icon-cache &>/dev/null; then
        gtk-update-icon-cache "$HOME/.local/share/icons/$_theme_name/" &>/dev/null
    fi
}

install_theme_icon hicolor
if command -v gsettings &>/dev/null; then
    THEME_NAME="$(gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null | tr -d "'\"")"
    if [ -n "$THEME_NAME" ] && [ "$THEME_NAME" != "hicolor" ]; then
        install_theme_icon "$THEME_NAME"
    fi
fi

echo "Done."
exit 0

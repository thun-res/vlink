#!/usr/bin/env bash

echo "Uninstall..."

_LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"

if [ -d "$HOME/Desktop" ]; then
    for _name in "VLink Viewer" "VLink Player" "VLink Analyzer" "VLink CMD"; do
        if [ -d "$HOME/Desktop/${_name}.app" ]; then
            [ -x "$_LSREGISTER" ] && "$_LSREGISTER" -u "$HOME/Desktop/${_name}.app" >/dev/null 2>&1
            rm -rf "$HOME/Desktop/${_name}.app" 2>/dev/null
        fi
    done

    if [ -L "$HOME/Desktop/VLink Player" ]; then
        rm -f "$HOME/Desktop/VLink Player" 2>/dev/null
    fi
fi

rm -f "$HOME/.config/vlink/install_path" 2>/dev/null
rmdir "$HOME/.config/vlink" 2>/dev/null

echo "Done."
exit 0

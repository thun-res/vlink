#compdef vlink-info info

# VLink zsh completion for vlink-info.

_vlink-info() {
    _arguments -s \
        '(-l --list_options)'{-l,--list_options}'[List options]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

if (( $+functions[compdef] )); then
    compdef _vlink-info vlink-info info
fi

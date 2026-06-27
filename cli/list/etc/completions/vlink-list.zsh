#compdef vlink-list list

# VLink zsh completion for vlink-list.

_vlink-list() {
    _arguments -s \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-m --name)'{-m,--name}'=[Process name]:name:' \
        '(-p --pid)'{-p,--pid}'=[Process id]:pid:' \
        '(-c --check_process_count)'{-c,--check_process_count}'[Get process count (By return value, no terminal output)]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

if (( $+functions[compdef] )); then
    compdef _vlink-list vlink-list list
fi

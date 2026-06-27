#compdef vlink-check check

# VLink zsh completion for vlink-check.

_vlink-check_diag() {
    _arguments -s \
        '(-a --all)'{-a,--all}'[All case]' \
        '(-s --summary)'{-s,--summary}'[Print PASSED/WARNING/FAILED counts at the end]' \
        '(-f --filter)'{-f,--filter}'[Only run checks whose title contains the given substring]:substring:' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-check_env() {
    _arguments -s \
        '(-b --available)'{-b,--available}'[Only available]' \
        '(-p --prefix)'{-p,--prefix}'[Only show variables whose name starts with the given prefix]:prefix:' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-check_test() {
    _arguments -s '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-check() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'diag:Start automatic diagnosis'
        'env:Detect environment variables'
        'test:Run an intra:// pub/sub self-test'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-check command' subcommands
            ;;
        args)
            case "$line[1]" in
                diag) _vlink-check_diag ;;
                env)  _vlink-check_env ;;
                test) _vlink-check_test ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-check vlink-check check
fi

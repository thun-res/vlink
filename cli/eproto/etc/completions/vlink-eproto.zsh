#compdef vlink-eproto eproto

# VLink zsh completion for vlink-eproto.

_vlink-eproto_pub() {
    _arguments -s \
        '(-d --proto_dir)'{-d,--proto_dir}'=[Proto dir]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '(-s --ser_type)'{-s,--ser_type}'=[Ser type]:ser:' \
        '(-x --encoding)'{-x,--encoding}'=[Encoding]:encoding:(protobuf flatbuffers raw blob zerocopy)' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-j --json)'{-j,--json}'[JSON format]' \
        '(-f --prototxt_file)'{-f,--prototxt_file}'=[Proto text file]:file:_files' \
        '(-c --prototxt_content)'{-c,--prototxt_content}'=[Proto text content]:content:' \
        '(-t --times)'{-t,--times}'=[Publish times]:times:' \
        '(-l --interval)'{-l,--interval}'=[Interval (ms)]:ms:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':url:_vlink_zsh_complete_url'
}

_vlink-eproto_sub() {
    _arguments -s \
        '(-d --proto_dir)'{-d,--proto_dir}'=[Proto dir]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '(-s --ser_type)'{-s,--ser_type}'=[Ser type]:ser:' \
        '(-x --encoding)'{-x,--encoding}'=[Encoding]:encoding:(protobuf flatbuffers raw blob zerocopy)' \
        '(-i --filter)'{-i,--filter}'=[Filter]:filter:' \
        '(-j --json)'{-j,--json}'[JSON format]' \
        '(-g --getter)'{-g,--getter}'[Getter mode]' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-m --max_str_count)'{-m,--max_str_count}'=[Max string count]:n:' \
        '(-e --print_enum_string)'{-e,--print_enum_string}'[Print enum strings]' \
        '(-r --ignore_array)'{-r,--ignore_array}'[Ignore arrays]' \
        '(-t --ignore_string)'{-t,--ignore_string}'[Ignore strings]' \
        '(-y --print_time_string)'{-y,--print_time_string}'[Print time strings]' \
        '(-u --print_hex_string)'{-u,--print_hex_string}'[Print hex strings]' \
        '(-o --ignore_default)'{-o,--ignore_default}'[Ignore default values]' \
        '(-p --use_long_repeated)'{-p,--use_long_repeated}'[Long repeated view]' \
        '--rows=[Rows]:rows:' \
        '--columns=[Columns]:cols:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':url:_vlink_zsh_complete_url'
}

_vlink-eproto_import() {
    _arguments -s \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':dir:_files -/'
}

_vlink-eproto() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'pub:Publish data'
        'sub:Subscribe data'
        'import:Save proto dir to $HOME/.vlink_proto_dir'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-eproto command' subcommands
            ;;
        args)
            case "$line[1]" in
                pub) _vlink-eproto_pub ;;
                sub) _vlink-eproto_sub ;;
                import) _vlink-eproto_import ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-eproto vlink-eproto eproto
fi

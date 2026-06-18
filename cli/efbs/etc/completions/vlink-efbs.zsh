#compdef vlink-efbs efbs

# VLink zsh completion for vlink-efbs.

_vlink-efbs_pub() {
    _arguments -s \
        '(-d --fbs_dir)'{-d,--fbs_dir}'=[FBS dir]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '(-s --ser_type)'{-s,--ser_type}'=[Ser type]:ser:' \
        '(-x --encoding)'{-x,--encoding}'=[Encoding]:encoding:(protobuf flatbuffers raw blob zerocopy)' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-f --fbstxt_file)'{-f,--fbstxt_file}'=[FBS text file]:file:_files' \
        '(-c --fbstxt_content)'{-c,--fbstxt_content}'=[FBS text content]:content:' \
        '(-t --times)'{-t,--times}'=[Publish times]:times:' \
        '(-l --interval)'{-l,--interval}'=[Interval (ms)]:ms:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':url:_vlink_zsh_complete_url'
}

_vlink-efbs_sub() {
    _arguments -s \
        '(-d --fbs_dir)'{-d,--fbs_dir}'=[FBS dir]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '(-s --ser_type)'{-s,--ser_type}'=[Ser type]:ser:' \
        '(-x --encoding)'{-x,--encoding}'=[Encoding]:encoding:(protobuf flatbuffers raw blob zerocopy)' \
        '(-i --filter)'{-i,--filter}'=[Filter]:filter:' \
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

_vlink-efbs_import() {
    _arguments -s \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':dir:_files -/'
}

_vlink-efbs() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'pub:Publish data'
        'sub:Subscribe data'
        'import:Save fbs dir to $HOME/.vlink_fbs_dir'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-efbs command' subcommands
            ;;
        args)
            case "$line[1]" in
                pub) _vlink-efbs_pub ;;
                sub) _vlink-efbs_sub ;;
                import) _vlink-efbs_import ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-efbs vlink-efbs efbs
fi

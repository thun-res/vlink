#compdef vlink-bag bag

# VLink zsh completion for vlink-bag.

_vlink-bag_bag_file() {
    _vlink_zsh_complete_bag_file
}

_vlink-bag_complete_url_list() {
    case "$1" in
        -u|--urls|--ignore_compress)
            _vlink_zsh_complete_url
            return 0
            ;;
    esac

    return 1
}

_vlink-bag_complete_actions_list() {
    case "$1" in
        -s|--actions)
            _values 'action' 1 2 3 4 5 6 7 8
            return 0
            ;;
    esac

    return 1
}

_vlink-bag_info() {
    _arguments -s \
        '(-l --detail)'{-l,--detail}'[Detail mode]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_vlink-bag_bag_file'
}

_vlink-bag_record() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-bag_complete_url_list "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-bag_complete_url_list "$last_option" && return
    fi

    _arguments -s \
        '*'{-u,--urls}'=[Bind urls, empty is all]:url:_vlink_zsh_complete_url' \
        '(-t --tag)'{-t,--tag}'=[Tag name]:tag:' \
        '(-i --filter)'{-i,--filter}'=[Filter regex]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-d --duration)'{-d,--duration}'=[Duration (s)]:duration:' \
        '(-w --wait)'{-w,--wait}'=[Wait time (s)]:wait:' \
        '(-p --compress)'{-p,--compress}'[Compress data]' \
        '(-f --force)'{-f,--force}'[Overwriting]' \
        '(-q --quiet)'{-q,--quiet}'[Quiet mode]' \
        '(-l --detail)'{-l,--detail}'[Detail mode]' \
        '(-o --split_name_by_time)'{-o,--split_name_by_time}'[Split name by time]' \
        '(-z --split_by_size)'{-z,--split_by_size}'=[Split by size (GB)]:size:' \
        '(-y --split_by_time)'{-y,--split_by_time}'=[Split by time (s)]:time:' \
        '(-g --deft)'{-g,--deft}'[No collect serialization infomation]' \
        '(-x --max_packet_size)'{-x,--max_packet_size}'=[Max packet size (MB)]:size:' \
        '(-j --wal_mode)'{-j,--wal_mode}'[Enable WAL mode]' \
        '(-c --cache_size)'{-c,--cache_size}'=[Cache size (MB)]:size:' \
        '(-s --sync_mode)'{-s,--sync_mode}'[Sync mode]' \
        '--max_task_depth=[Max pending tasks in the queue]:depth:' \
        '--max_memory_size=[Max memory size in the queue (GB)]:size:' \
        '--max_row_count=[Max row count]:count:' \
        '--max_bytes_size=[Max bytes size (GB)]:size:' \
        '--enable_limit[Enable limits]' \
        '--compress_level=[Compress level (1-5)]:level:(0 1 2 3 4 5)' \
        '*--ignore_compress=[Ignore compress urls (repeatable)]:url:_vlink_zsh_complete_url' \
        '--plugin=[Record plugin (rewrites frames on write)]:plugin:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_files'
}

_vlink-bag_play() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-bag_complete_url_list "${words[CURRENT-1]}" && return
    _vlink-bag_complete_actions_list "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-bag_complete_url_list "$last_option" && return
        _vlink-bag_complete_actions_list "$last_option" && return
    fi

    _arguments -s \
        '*'{-u,--urls}'=[Bind urls, empty is all]:url:_vlink_zsh_complete_url' \
        '(-i --filter)'{-i,--filter}'=[Filter regex]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '*'{-s,--actions}'=[1\:C/Req 2\:C/Resp 3\:S/Req 4\:S/Resp 5\:Pub 6\:Sub 7\:Set 8\:Get (repeatable)]:action:(1 2 3 4 5 6 7 8)' \
        '(-b --begin_time)'{-b,--begin_time}'=[Begin time (s)]:time:' \
        '(-e --end_time)'{-e,--end_time}'=[End time (s)]:time:' \
        '(-t --times)'{-t,--times}'=[Play times (<= 0 means infinite)]:times:' \
        '(-r --rate)'{-r,--rate}'=[Playback rate 0.01-100]:rate:' \
        '(-q --quiet)'{-q,--quiet}'[Quiet mode]' \
        '(-l --detail)'{-l,--detail}'[Detail mode]' \
        '(-m --skip_blank)'{-m,--skip_blank}'[Skip blank]' \
        '(-j --auto_pause)'{-j,--auto_pause}'[Auto pause]' \
        '--local_time[Use local time]' \
        '--utc_time[Use UTC time]' \
        '--rel_begin_time=[Rel begin HH:MM:SS]:time:' \
        '--rel_end_time=[Rel end HH:MM:SS]:time:' \
        '--local_begin_time=[Local begin HH:MM:SS]:time:' \
        '--local_end_time=[Local end HH:MM:SS]:time:' \
        '--utc_begin_time=[UTC begin HH:MM:SS]:time:' \
        '--utc_end_time=[UTC end HH:MM:SS]:time:' \
        '--plugin=[Playback plugin]:plugin:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_vlink-bag_bag_file'
}

_vlink-bag_clone() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-bag_complete_url_list "${words[CURRENT-1]}" && return
    _vlink-bag_complete_actions_list "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-bag_complete_url_list "$last_option" && return
        _vlink-bag_complete_actions_list "$last_option" && return
    fi

    _arguments -s \
        '*'{-u,--urls}'=[Bind urls, empty is all]:url:_vlink_zsh_complete_url' \
        '(-t --tag)'{-t,--tag}'=[Tag name]:tag:' \
        '(-i --filter)'{-i,--filter}'=[Filter regex]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '*'{-s,--actions}'=[Action types 1\:C/Req 2\:C/Resp 3\:S/Req 4\:S/Resp 5\:Pub 6\:Sub 7\:Set 8\:Get (repeatable)]:action:(1 2 3 4 5 6 7 8)' \
        '(-b --begin_time)'{-b,--begin_time}'=[Begin time (s)]:time:' \
        '(-e --end_time)'{-e,--end_time}'=[End time (s)]:time:' \
        '(-q --quiet)'{-q,--quiet}'[Quiet mode]' \
        '(-l --detail)'{-l,--detail}'[Detail mode]' \
        '(-p --compress)'{-p,--compress}'[Compress data]' \
        '(-o --split_name_by_time)'{-o,--split_name_by_time}'[Split name by time]' \
        '(-z --split_by_size)'{-z,--split_by_size}'=[Split by size]:size:' \
        '(-y --split_by_time)'{-y,--split_by_time}'=[Split by time]:time:' \
        '(-f --force)'{-f,--force}'[Overwriting]' \
        '(-j --wal_mode)'{-j,--wal_mode}'[Enable WAL mode]' \
        '(-c --cache_size)'{-c,--cache_size}'=[Cache size (MB)]:size:' \
        '--rel_begin_time=[Rel begin HH:MM:SS]:time:' \
        '--rel_end_time=[Rel end HH:MM:SS]:time:' \
        '--local_begin_time=[Local begin HH:MM:SS]:time:' \
        '--local_end_time=[Local end HH:MM:SS]:time:' \
        '--utc_begin_time=[UTC begin HH:MM:SS]:time:' \
        '--utc_end_time=[UTC end HH:MM:SS]:time:' \
        '--compress_level=[Compress level]:level:(0 1 2 3 4 5)' \
        '*--ignore_compress=[Ignore compress urls (repeatable)]:url:_vlink_zsh_complete_url' \
        '--import_schema[Import schema]' \
        '--plugin=[Clone plugin (rewrites frames on write)]:plugin:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':source bag:_vlink-bag_bag_file' \
        ':target bag:_files'
}

_vlink-bag_fix() {
    _arguments -s \
        '(-y --rebuild)'{-y,--rebuild}'[Rebuild]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_vlink-bag_bag_file'
}

_vlink-bag_simple() {
    _arguments -s \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_vlink-bag_bag_file'
}

_vlink-bag_tag() {
    _arguments -s \
        '(-h --help)'{-h,--help}'[Show help]' \
        ':bag path:_vlink-bag_bag_file' \
        ':tag name:'
}

_vlink-bag() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'info:Show bag info'
        'record:Record bag from live topics'
        'play:Replay bag'
        'clone:Clone bag to another bag'
        'check:Check bag integrity'
        'reindex:Rebuild bag index'
        'fix:Fix bag'
        'tag:Write tag to bag'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-bag command' subcommands
            ;;
        args)
            case "$line[1]" in
                info) _vlink-bag_info ;;
                record) _vlink-bag_record ;;
                play) _vlink-bag_play ;;
                clone) _vlink-bag_clone ;;
                check) _vlink-bag_simple ;;
                reindex) _vlink-bag_simple ;;
                fix) _vlink-bag_fix ;;
                tag) _vlink-bag_tag ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-bag vlink-bag bag
fi

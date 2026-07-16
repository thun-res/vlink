#compdef vlink-dump dump

# VLink zsh completion for vlink-dump.

_vlink-dump_complete_multi() {
    case "$1" in
        -u|--urls|--ignore_compress)
            _vlink_zsh_complete_url
            return 0
            ;;
        --actions)
            _values 'action type' 0 1 2 3 4 5 6 7 8
            return 0
            ;;
    esac

    return 1
}

_vlink-dump() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-dump_complete_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-dump_complete_multi "$last_option" && return
    fi

    _arguments -s \
        '(-t --type)'{-t,--type}'=[Mode/output format]:format:(console text csv json bin jpg jpeg h264 h265 raw pcd slice scan)' \
        '(-c --condition)'{-c,--condition}'=[Field list, comma-separated or quoted space-separated]:fields:' \
        '(-o --out_dir)'{-o,--out_dir}'=[Output directory]:dir:_files -/' \
        '(-m --base_name)'{-m,--base_name}'=[Output base name]:name:' \
        '(-f --bag_file --native)'{-f,--bag_file}'=[Bag source]:bag:_vlink_zsh_complete_play_file' \
        '(-b --begin_time --native)'{-b,--begin_time}'=[Begin time in seconds]:time:' \
        '(-e --end_time --native)'{-e,--end_time}'=[End time in seconds]:time:' \
        '(-n --count)'{-n,--count}'=[Max count]:count:' \
        '--hz=[Max frequency]:hz:' \
        '(-f --bag_file -b --begin_time -e --end_time)--native[Native mode (mutually exclusive with -f/-b/-e)]' \
        '(-d --proto_dir)'{-d,--proto_dir}'=[Proto dir]:dir:_files -/' \
        '--fbs_dir=[FBS dir]:dir:_files -/' \
        '(-q --quiet)'{-q,--quiet}'[Quiet mode]' \
        '(-l --detail)'{-l,--detail}'[Detail mode]' \
        '*'{-x,--expression}'=[Math expression; repeat for multiple expressions]:expr:' \
        '(-w --window)'{-w,--window}'=[Slice window in seconds]:seconds:' \
        '--segments=[Segment JSON file]:file:_files' \
        '--event=[Event expression for scan/slice]:expr:' \
        '--pre=[Seconds before event]:seconds:' \
        '--post=[Seconds after event]:seconds:' \
        '--event_state_max_age=[Cross-topic state max age in seconds]:seconds:' \
        '--event_min_interval=[Minimum seconds between event triggers]:seconds:' \
        '--suffix=[Slice output suffix]:suffix:' \
        '--compress[Compress slice output bag data]' \
        '--force[Overwrite existing slice/scan outputs]' \
        '--no_manifest[Skip slice manifest]' \
        '--manifest=[Manifest file name]:name:' \
        '--scan_output=[Scan result JSON file name]:name:' \
        '--schema_config=[Schema import config JSON]:file:_files' \
        '--schema_plugin=[Schema plugin .so path]:file:_files' \
        '--plugin=[Bag plugin name (rewrites frames on read)]:plugin:' \
        '--filter=[Slice content filter expression]:expr:' \
        '--export_csv[Export slice field CSV sidecars]' \
        '*'{-u,--urls}'=[Exact URL filter]:url:_vlink_zsh_complete_url' \
        '(-i --url_filter)'{-i,--url_filter}'=[Quoted URL keywords]:keywords:' \
        '(-k --black)'{-k,--black}'[Blacklist mode for URL filters]' \
        '*--actions=[Action type filter]:action:(0 1 2 3 4 5 6 7 8)' \
        '--tag=[Output bag tag]:tag:' \
        '--wal_mode[Enable WAL mode for slice outputs]' \
        '--cache_size=[Writer cache size in MB]:size:' \
        '--compress_level=[Compress level 0-5]:level:(0 1 2 3 4 5)' \
        '*--ignore_compress=[URL to skip compression]:url:_vlink_zsh_complete_url' \
        '--sample_step=[Keep every Nth message per URL]:step:' \
        '--dry_run[Show slice plan without writing]' \
        '--quality_check[Enable scan quality checks]' \
        '--dropout_threshold=[Dropout threshold in seconds]:seconds:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        ':url (optional for slice/scan):_vlink_zsh_complete_url'
}

if (( $+functions[compdef] )); then
    _vlink_zsh_register_completion _vlink-dump vlink-dump dump
fi

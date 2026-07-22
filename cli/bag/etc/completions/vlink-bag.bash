# VLink bash completion for vlink-bag.

_vlink_bag_positional_count() {
    local subcommand="$1"
    local index count=0 expect=""

    for ((index=1; index<COMP_CWORD; index++)); do
        local token="${COMP_WORDS[index]}"

        if [[ "$token" == "$subcommand" ]]; then
            continue
        fi

        case "$expect" in
            multi)
                if [[ "$token" == -* ]]; then
                    expect=""
                    ((index--))
                fi
                continue
                ;;
            single)
                expect=""
                continue
                ;;
        esac

        case "$token" in
            -u|--urls|--ignore_compress)
                expect="multi"
                continue
                ;;
            -s|--actions)
                case "$subcommand" in
                    play|clone)
                        expect="multi"
                        continue
                        ;;
                esac
                ;;
            -t|--tag|-i|--filter|-d|--duration|-w|--wait|-z|--split_by_size|-y|--split_by_time|\
            -x|--max_packet_size|-c|--cache_size|--max_task_depth|--max_memory_size|--max_row_count|\
            --max_bytes_size|--compress_level|--plugin|-b|--begin_time|-e|--end_time|--rel_begin_time|\
            --rel_end_time|--local_begin_time|--local_end_time|--utc_begin_time|--utc_end_time|-r|--rate|\
            --times)
                expect="single"
                continue
                ;;
            -*)
                continue
                ;;
            *)
                ((count++))
                ;;
        esac
    done

    printf '%s' "$count"
}

_vlink_bag() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcommands="info record play clone check reindex fix tag"
    local subcommand
    local last_option=""

    COMPREPLY=()
    subcommand=$(_vlink_bash_find_subcommand "$subcommands")

    if [[ -z "$subcommand" ]]; then
        _vlink_bash_complete_words "$subcommands -h --help -v --version" "$cur"
        return
    fi

    case "$prev" in
        -u|--urls|--ignore_compress)
            _vlink_bash_complete_url "$cur"
            return
            ;;
        -t|--tag|-i|--filter|-d|--duration|-w|--wait|-z|--split_by_size|-y|--split_by_time|\
        -x|--max_packet_size|-c|--cache_size|--max_task_depth|--max_memory_size|--max_row_count|\
        --max_bytes_size|--compress_level|--plugin|-b|--begin_time|-e|--end_time|--rel_begin_time|\
        --rel_end_time|--local_begin_time|--local_end_time|--utc_begin_time|--utc_end_time|-r|--rate|\
        --times)
            return
            ;;
    esac

    if [[ "$prev" == "-s" || "$prev" == "--actions" ]]; then
        case "$subcommand" in
            play|clone)
                _vlink_bash_complete_words "1 2 3 4 5 6 7 8" "$cur"
                return
                ;;
        esac
    fi

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        case "$last_option" in
            -u|--urls|--ignore_compress)
                _vlink_bash_complete_url "$cur"
                return
                ;;
            -s|--actions)
                case "$subcommand" in
                    play|clone)
                        _vlink_bash_complete_words "1 2 3 4 5 6 7 8" "$cur"
                        return
                        ;;
                esac
                ;;
        esac
    fi

    case "$subcommand" in
        info)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-l --detail -h --help" "$cur"
                return
            fi
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            ;;
        record)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-u --urls -t --tag -i --filter -k --black -n --native \
-d --duration -w --wait -p --compress -f --force -q --quiet -l --detail \
-o --split_name_by_time -z --split_by_size -y --split_by_time -g --deft \
-x --max_packet_size -j --wal_mode -c --cache_size -s --sync_mode \
--max_task_depth --max_memory_size --max_row_count --max_bytes_size \
--enable_limit --compress_level --ignore_compress --plugin -h --help" "$cur"
                return
            fi
            _vlink_bash_complete_files "$cur"
            ;;
        play)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-u --urls -i --filter -k --black -n --native -s --actions \
-b --begin_time -e --end_time -t --times -r --rate -q --quiet -l --detail \
-m --skip_blank -j --auto_pause --local_time --utc_time \
--rel_begin_time --rel_end_time --local_begin_time --local_end_time \
--utc_begin_time --utc_end_time --plugin -h --help" "$cur"
                return
            fi
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            ;;
        clone)
            local pos_count

            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-u --urls -t --tag -i --filter -k --black -s --actions \
-b --begin_time -e --end_time -q --quiet -l --detail -p --compress \
-o --split_name_by_time -z --split_by_size -y --split_by_time \
-f --force -j --wal_mode -c --cache_size \
--rel_begin_time --rel_end_time --local_begin_time --local_end_time \
--utc_begin_time --utc_end_time --compress_level --ignore_compress \
--import_schema --plugin -h --help" "$cur"
                return
            fi

            pos_count=$(_vlink_bag_positional_count "$subcommand")
            if (( pos_count == 0 )); then
                _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            else
                _vlink_bash_complete_files "$cur"
            fi
            ;;
        check|reindex)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-h --help" "$cur"
                return
            fi
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            ;;
        fix)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-y --rebuild -h --help" "$cur"
                return
            fi
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            ;;
        tag)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-h --help" "$cur"
                return
            fi
            (( COMP_CWORD > 2 )) && return
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_bag_ext"
            ;;
    esac
}

_vlink_bash_register_completion _vlink_bag vlink-bag bag

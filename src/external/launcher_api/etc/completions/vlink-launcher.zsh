#compdef vlink-launcher launcher

# VLink zsh completion for vlink-launcher.

_vlink-launcher() {
    local -a subcmds ctl_actions

    subcmds=(
        'run:Launch a VLink system from a TOML manifest'
        'validate:Validate TOML manifest, dependencies, conflicts, slices and DAG acyclicity'
        'graph:Export resolved startup DAG in ASCII, DOT or Mermaid format'
        'inspect:Inspect resolved manifest, arguments, groups and startup layers'
        'logs:Inspect persisted .log files from a launcher session directory offline'
        'plugin:Inspect exported component or logger plugin interfaces in a shared library'
        'init:Generate a starter TOML launch manifest'
        'top:Interactive real-time TUI dashboard for a running LauncherServer'
        'ctl:Inspect and control a running LauncherServer'
        'completion:Print bash or zsh completion script'
        '-h:Show help'
        '--help:Show help'
        '-v:Show version'
        '--version:Show version'
    )

    ctl_actions=(
        'status:Show cluster or unit status'
        'list-units:List units with optional filters'
        'list-dependencies:Show forward or reverse dependency tree'
        'blame:Rank units by cold-boot startup latency'
        'critical-chain:Show slowest startup critical path'
        'cat:Print resolved effective unit configuration'
        'start:Start a unit or @group'
        'stop:Stop a unit or @group'
        'restart:Restart a unit or @group'
        'reload:Reload a component or send SIGHUP to a process'
        'activate:Transition an inactive component to ready'
        'deactivate:Transition a ready component to inactive standby'
        'reset-failed:Clear failed state and start-limit counters'
        'isolate:Switch exclusively to @group and its dependency closure'
        'resume:Resume startup queue after --pause_on_fail'
        'load:Dynamically load a component plugin into host or container'
        'unload:Dynamically unload a component plugin'
        'params:Query global or component parameters'
        'set-param:Update a component or global parameter at runtime'
        'record:Start or stop active bag recording on components'
        'events:Stream lifecycle events'
        'logs:Query and stream unit logs'
        'shutdown:Gracefully stop the LauncherServer'
    )

    if (( CURRENT == 2 )); then
        _describe 'command' subcmds
        return
    fi

    case "${words[2]}" in
        run)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '*--arg=[Override launch argument k=v]:kv:' \
                '--dry_run[Print execution plan without running]' \
                '--show_args[List declared launch arguments]' \
                '--serial[Start units strictly one at a time]' \
                '--parallel=[Max concurrent unit starts]:count:' \
                '*--only=[Start only unit/@group + deps]:target:' \
                '*--skip=[Skip unit or @group]:target:' \
                '*--debug_unit=[Wrap unit with prefix]:spec:' \
                '--no_respawn[Disable automatic respawn]' \
                '--pause_on_fail[Pause startup queue after first failure]' \
                '--log_level=[Override console log level]:level:(trace debug info warn error fatal off)' \
                '--log_plugin=[Override LoggerPluginInterface (logd) library]:plugin:(logd)' \
                '--no_control[Disable control-plane endpoints]' \
                '--plain[Disable ANSI colors]' \
                '--log_dir=[Override session log directory]:dir:_files -/' \
                '--record_dir=[Enable session active recording into directory]:dir:_files -/' \
                '--scheme=[Control-plane scheme]:scheme:(dds ddsc ddsr zenoh intra)' \
                '--namespace=[Control-plane namespace prefix]:ns:' \
                '--domain=[Control-plane domain ID]:id:' \
                '--key=[AES-128-GCM security key]:key:' \
                '--bind_ip=[Transport bind IP]:ip:' \
                '--native[Restrict discovery to local host]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        validate)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '*--arg=[Override launch argument k=v]:kv:' \
                '--check_programs[Verify executables exist]' \
                '--json[Output validation report as JSON]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        graph)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '--format=[Output format]:format:(ascii dot mermaid)' \
                '(-o --out)'{-o,--out}'=[Output file path]:file:_files' \
                '*--arg=[Override launch argument k=v]:kv:' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        inspect)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '--show_args[Show argument table]' \
                '--json[Emit full resolved JSON]' \
                '*--arg=[Override launch argument k=v]:kv:' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        plugin)
            _arguments -s \
                '*:Plugin shared library:_files -g "*.(so|dylib|dll)"' \
                '--json[Emit JSON metadata]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        init)
            _arguments -s \
                '--template=[Starter template]:template:(minimal component systemd)' \
                '(-o --out)'{-o,--out}'=[Output file path]:file:_files' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        top)
            _arguments -s \
                '--scheme=[Control-plane scheme]:scheme:(dds ddsc ddsr zenoh intra)' \
                '--namespace=[Control-plane namespace prefix]:ns:' \
                '--domain=[Control-plane domain ID]:id:' \
                '--key=[AES-128-GCM security key]:key:' \
                '--bind_ip=[Transport bind IP]:ip:' \
                '--native[Restrict discovery to local host]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        logs)
            _arguments -s \
                '*:Session log directory:_files -/' \
                '(-u --unit)'{-u,--unit}'=[Filter by unit name]:unit:' \
                '--grep=[Substring filter]:pattern:' \
                '(-n --lines)'{-n,--lines}'=[Tail N lines]:count:' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        completion)
            if (( CURRENT == 3 )); then
                _values 'shell' bash zsh
            fi
            ;;
        ctl)
            if (( CURRENT == 3 )); then
                _describe 'ctl action' ctl_actions
                return
            fi
            if [[ "${words[3]}" == "record" ]] && (( CURRENT == 4 )); then
                _values 'record action' start stop
                return
            fi
            _arguments -s \
                '(-u --unit)'{-u,--unit}'=[Target unit or @group]:target:' \
                '--type=[Filter by unit type]:type:(component process container task proxy)' \
                '--state=[Filter by unit state]:state:(pending starting running ready inactive stopping stopped failed restarting skipped)' \
                '--group=[Filter by group]:group:' \
                '--failed[Show failed units only]' \
                '--reverse[Show reverse dependencies]' \
                '(-w --watch)'{-w,--watch}'[Continuously refresh status]' \
                '--json[JSON output]' \
                '(-n --lines)'{-n,--lines}'=[Tail N log records]:count:' \
                '(-f --follow)'{-f,--follow}'[Follow live log stream]' \
                '--level=[Min log level]:level:(trace debug info warn error fatal)' \
                '--grep=[Substring filter]:pattern:' \
                '--stderr[Show stderr logs only]' \
                '--lib=[Shared library for load]:lib:' \
                '--class=[Component class name for load]:cls:' \
                '--scheduler=[Scheduler group for load]:sched:' \
                '--compute_pool=[Compute ThreadPool group for load]:pool:' \
                '--container=[Target container for load]:container:' \
                '--worker=[Pinned worker index]:idx:' \
                '--priority=[Default task priority]:prio:' \
                '--legacy[Load legacy RunablePluginInterface]' \
                '*--param=[Initial param k=v]:kv:' \
                '*--url=[Named URL k=url]:kv:' \
                '*--input=[Input port k=url]:kv:' \
                '*--output=[Output port k=url]:kv:' \
                '--param_key=[Parameter key]:key:' \
                '--param_val=[Parameter value]:val:' \
                '--param_type=[Param type]:type:(auto bool int double string bool_list int_list double_list string_list)' \
                '--path=[Bag output path for ctl record]:file:_files -g "*.(vcap|vcapx|vdb|vdbx)"' \
                '--stop[Stop active recording for ctl record]' \
                '--timeout=[RPC timeout ms]:ms:' \
                '--scheme=[Control-plane scheme]:scheme:(dds ddsc ddsr zenoh intra)' \
                '--namespace=[Control-plane namespace prefix]:ns:' \
                '--domain=[Control-plane domain ID]:id:' \
                '--key=[AES-128-GCM security key]:key:' \
                '--bind_ip=[Transport bind IP]:ip:' \
                '--native[Restrict discovery to local host]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
    esac
}

if (( $+functions[compdef] )); then
    _vlink_zsh_register_completion _vlink-launcher vlink-launcher launcher
fi

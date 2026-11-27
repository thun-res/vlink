#compdef vlink-launcher launcher

# VLink zsh completion for vlink-launcher.

_vlink-launcher() {
    local -a subcmds

    subcmds=(
        'run:Launch a VLink system from a TOML manifest'
        'host:Run a component manifest inside an isolated host process bridged to LauncherNotify'
        'validate:Validate TOML manifest, dependencies and DAG acyclicity'
        'exec:Run a LauncherComponent plugin shared library standalone'
        'plugin:List exported LauncherComponent classes in a shared library'
        'status:Show unit status from a running LauncherServer'
        'events:Stream live unit lifecycle events from a running LauncherServer'
        'start:Start a unit or @group'
        'stop:Stop a unit or @group'
        'restart:Restart a unit or @group'
        'reload:Reload a component or send SIGHUP to a process'
        'params:Query component or unit parameters'
        'set-param:Update a component parameter at runtime'
        'shutdown:Gracefully stop the LauncherServer'
        'help:Show help'
        '-h:Show help'
        '--help:Show help'
        '-v:Show version'
        '--version:Show version'
    )

    if (( CURRENT == 2 )); then
        _describe 'command' subcmds
        return
    fi

    case "${words[2]}" in
        run|host)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '*--arg[Override manifest argument k=v]:kv:' \
                '*--only[Start only unit/@group + dependencies]:target:' \
                '*--skip[Skip unit or @group]:target:' \
                '--dry-run[Print resolved DAG startup order without launching]' \
                '--scheme[Control-plane scheme]:scheme:(dds ddsc ddsr zenoh intra)' \
                '--ns[Control-plane namespace prefix]:ns:' \
                '--domain[Control-plane domain ID]:id:' \
                '--key[AES-128-GCM security key]:key:' \
                '--no-ctrl[Disable control-plane endpoints]' \
                '--no-respawn[Disable automatic respawn]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        validate)
            _arguments -s \
                '*:TOML manifest:_files -g "*.toml"' \
                '*--arg[Override manifest argument k=v]:kv:' \
                '--check-programs[Verify process executables exist]' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        exec)
            _arguments -s \
                '1:Plugin shared library:_files -g "*.(so|dylib|dll)"' \
                '2::Component class name:' \
                '(-n --name)'{-n,--name}'[Unit name]:name:' \
                '--ns[Unit namespace]:ns:' \
                '*'{-p,--param}'[Initial parameter k=v]:kv:' \
                '*'{-r,--remap}'[URL remap src=dst]:kv:' \
                '--scheduler[Scheduler mode]:mode:(dedicated multi)' \
                '--threads[MultiLoop worker threads]:num:' \
                '--queue[Queue backend]:queue:(normal lockfree priority)' \
                '--queue-strategy[Queue back-pressure strategy]:strategy:(optimization pop block)' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        plugin)
            _arguments -s \
                '1:Plugin shared library:_files -g "*.(so|dylib|dll)"' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
        status|events|start|stop|restart|reload|params|set-param|shutdown)
            _arguments -s \
                '--scheme[Control-plane scheme]:scheme:(dds ddsc ddsr zenoh intra)' \
                '--ns[Control-plane namespace prefix]:ns:' \
                '--domain[Control-plane domain ID]:id:' \
                '--key[AES-128-GCM security key]:key:' \
                '--timeout[RPC timeout in ms]:ms:' \
                '(-h --help)'{-h,--help}'[Show help]'
            ;;
    esac
}

if (( $+functions[compdef] )); then
    _vlink_zsh_register_completion _vlink-launcher vlink-launcher launcher
fi

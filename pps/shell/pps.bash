# pps interactive Bash integration. Source this file from ~/.bashrc.

if [[ -z ${BASH_VERSION-} ]]; then
    printf '%s\n' 'pps: unsupported shell' 'pps requires Bash.' >&2
    return 1 2>/dev/null || exit 1
fi

_pps_source=${BASH_SOURCE[0]}
_pps_dir=${_pps_source%/*}
if [[ $_pps_dir == "$_pps_source" ]]; then
    _pps_dir=.
fi
PPS_CORE=$(cd -- "$_pps_dir/.." 2>/dev/null && pwd -P)/bin/pps-core
unset _pps_source _pps_dir

pps() {
    if [[ $- != *i* ]]; then
        printf '%s\n' 'pps: unsupported shell or Bash integration is not loaded.' \
            'pps requires interactive Bash integration.' >&2
        return 2
    fi
    if [[ ! -x $PPS_CORE ]]; then
        printf 'pps: core executable not found: %s\n' "$PPS_CORE" >&2
        printf '%s\n' 'Run make in the pps source directory.' >&2
        return 127
    fi

    local result status kind selected command prompt arg
    result=$("$PPS_CORE" "$@")
    status=$?
    if (( status != 0 )); then
        return "$status"
    fi

    case $result in
        $'PPS_SELECT\t'*)
            kind=${result%%$'\t'*}
            selected=${result#*$'\t'}
            [[ $selected =~ ^[0-9]+$ ]] || {
                printf '%s\n' 'pps: invalid response from core' >&2
                return 1
            }
            printf -v command 'pps %q' "$selected"
            if (( $# > 0 )) && [[ $1 != -* ]]; then
                shift
            fi
            for arg in "$@"; do
                printf -v command '%s %q' "$command" "$arg"
            done
            prompt=${PS1@P}
            IFS= read -e -r -i "$command" -p "$prompt" command || return $?
            [[ -n $command ]] || return 0
            history -s -- "$command"
            builtin eval -- "$command"
            ;;
        $'PPS_SETVAR\t'*)
            selected=${result#*$'\t'}
            [[ $selected =~ ^[0-9]+$ ]] || {
                printf '%s\n' 'pps: invalid response from core' >&2
                return 1
            }
            export -n PID 2>/dev/null || true
            printf -v PID '%s' "$selected"
            ;;
        *)
            [[ -z $result ]] || printf '%s\n' "$result"
            ;;
    esac
}

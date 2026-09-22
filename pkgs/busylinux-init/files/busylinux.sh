# Wayland compositors, PipeWire and the XDG portals all need a runtime
# directory owned by the user.
if [ -z "$XDG_RUNTIME_DIR" ]; then
    XDG_RUNTIME_DIR=/run/user/$(id -u)
    mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null && chmod 0700 "$XDG_RUNTIME_DIR"
    # -O is a BusyBox ash builtin: true when the caller owns the directory.
    # shellcheck disable=SC3067
    if [ -O "$XDG_RUNTIME_DIR" ]; then
        export XDG_RUNTIME_DIR
    else
        unset XDG_RUNTIME_DIR
    fi
fi

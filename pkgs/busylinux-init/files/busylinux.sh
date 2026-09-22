if [ -z "$XDG_RUNTIME_DIR" ]; then
    XDG_RUNTIME_DIR=/run/user/$(id -u)
    mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null && chmod 0700 "$XDG_RUNTIME_DIR"
    if [ "$(stat -c %u "$XDG_RUNTIME_DIR" 2>/dev/null)" = "$(id -u)" ]; then
        export XDG_RUNTIME_DIR
    else
        unset XDG_RUNTIME_DIR
    fi
fi

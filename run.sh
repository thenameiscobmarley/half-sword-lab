#!/usr/bin/env bash
# Half Sword Lab launcher: checks what's missing, asks before installing anything, then opens the GUI.
#   ./run.sh          normal mode (finds Half Sword in your Steam libraries)
#   ./run.sh --demo   try the GUI with fake data, no game needed
#   ./run.sh --yes    don't ask (for scripts)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

say()  { printf '\033[1;35m[half-sword-lab]\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
py_ok() { "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; }

ASSUME_YES=${HSL_YES:-0}
ARGS=()
for a in "$@"; do
    if [ "$a" = "--yes" ] || [ "$a" = "-y" ]; then ASSUME_YES=1; else ARGS+=("$a"); fi
done
SUDO=""; [ "$(id -u)" -ne 0 ] && have sudo && SUDO="sudo"

PM=""
for p in pacman apt-get dnf zypper apk xbps-install; do have "$p" && { PM=$p; break; }; done
pm_install() {  # package names per manager
    case "$PM" in
        pacman) $SUDO pacman -S --needed --noconfirm "$@" ;;
        apt-get) $SUDO apt-get update && $SUDO apt-get install -y "$@" ;;
        dnf) $SUDO dnf install -y "$@" ;;
        zypper) $SUDO zypper --non-interactive install "$@" ;;
        apk) $SUDO apk add "$@" ;;
        xbps-install) $SUDO xbps-install -Sy "$@" ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------- work out what's missing
PY=""
for cand in python3 python; do
    if have "$cand" && py_ok "$cand"; then PY="$(command -v "$cand")"; break; fi
done

PLAN=()        # human-readable lines
NEED_PY=0 NEED_VENV_PKG=0 NEED_VENV=0 NEED_DEPS=0
if [ -z "$PY" ]; then
    NEED_PY=1
    if [ -n "$PM" ]; then PLAN+=("Python 3 from your system's package manager ($PM, needs your password)")
    else PLAN+=("Python 3.12 via uv (downloaded from astral.sh into ~/.local, no password needed)"); fi
elif ! "$PY" -c 'import venv, ensurepip' 2>/dev/null; then
    NEED_VENV_PKG=1
    PLAN+=("python3-venv from $PM (Debian/Mint ship Python without it; needs your password)")
fi
if [ ! -x .venv/bin/python ]; then
    NEED_VENV=1
    PLAN+=("a private Python environment in $(pwd)/.venv (nothing touches your system Python)")
fi
STAMP=.venv/.requirements.sha
SUM="$(sha256sum requirements.txt | cut -d' ' -f1)"
if [ "$(cat "$STAMP" 2>/dev/null)" != "$SUM" ]; then
    NEED_DEPS=1
    PLAN+=("Python packages from PyPI into .venv: $(grep -v '^#' requirements.txt | tr '\n' ' ')")
fi

if [ ${#PLAN[@]} -gt 0 ]; then
    say "Half Sword Lab needs to install:"
    for line in "${PLAN[@]}"; do printf '    • %s\n' "$line"; done
    if [ "$ASSUME_YES" != 1 ]; then
        if [ ! -t 0 ]; then say "No terminal to ask in; re-run with --yes to accept."; exit 1; fi
        read -r -p "$(printf '\033[1;35m[half-sword-lab]\033[0m Install these? [y/N] ')" answer
        case "$answer" in y|Y|yes|YES) ;; *) say "Nothing installed. Bye."; exit 0 ;; esac
    fi
fi

# ---------------------------------------------------------------- install
if [ "$NEED_PY" = 1 ]; then
    if [ -n "$PM" ]; then
        if [ "$PM" = pacman ]; then pm_install python || true; else pm_install python3 || true; fi
        for cand in python3 python; do have "$cand" && py_ok "$cand" && { PY="$(command -v "$cand")"; break; }; done
    fi
    if [ -z "$PY" ]; then
        say "Falling back to uv for a standalone Python..."
        have uv || curl -LsSf https://astral.sh/uv/install.sh | sh
        export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
        uv python install 3.12
        PY="$(uv python find 3.12)"
    fi
fi
if [ "$NEED_VENV_PKG" = 1 ]; then
    pm_install python3-venv || say "Couldn't install python3-venv; trying anyway."
fi
if [ "$NEED_VENV" = 1 ]; then
    say "Creating .venv ..."
    "$PY" -m venv .venv || { have uv && uv venv --python "$PY" .venv; }
fi
if [ "$NEED_DEPS" = 1 ]; then
    say "Installing Python packages ..."
    if .venv/bin/python -m pip --version >/dev/null 2>&1; then
        .venv/bin/python -m pip install --quiet --upgrade pip
        .venv/bin/python -m pip install --quiet -r requirements.txt
    else
        uv pip install --python .venv/bin/python -r requirements.txt
    fi
    echo "$SUM" > "$STAMP"
fi

exec .venv/bin/python -m halfsword_lab "${ARGS[@]+"${ARGS[@]}"}"

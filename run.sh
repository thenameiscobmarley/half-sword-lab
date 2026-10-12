#!/usr/bin/env bash
# Half Sword Lab launcher: gets Python + dependencies, then opens the terminal GUI.
#   ./run.sh          normal mode (finds Half Sword in your Steam libraries)
#   ./run.sh --demo   try the GUI with fake data, no game needed
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

say()  { printf '\033[1;35m[half-sword-lab]\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
SUDO=""; [ "$(id -u)" -ne 0 ] && have sudo && SUDO="sudo"

py_ok() { "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; }

install_system_python() {
    say "Python 3.10+ not found, installing it..."
    if   have pacman;  then $SUDO pacman -S --needed --noconfirm python
    elif have apt-get; then $SUDO apt-get update && $SUDO apt-get install -y python3 python3-venv python3-pip
    elif have dnf;     then $SUDO dnf install -y python3 python3-pip
    elif have zypper;  then $SUDO zypper --non-interactive install python3 python3-pip
    elif have apk;     then $SUDO apk add python3 py3-pip
    elif have xbps-install; then $SUDO xbps-install -Sy python3
    else return 1; fi
}

install_uv_python() {
    say "Falling back to uv to fetch a standalone Python (no root needed)..."
    have uv || curl -LsSf https://astral.sh/uv/install.sh | sh
    export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
    uv python install 3.12
    PY="$(uv python find 3.12)"
}

PY=""
for cand in python3 python; do
    if have "$cand" && py_ok "$cand"; then PY="$(command -v "$cand")"; break; fi
done
if [ -z "$PY" ]; then
    { install_system_python && have python3 && py_ok python3 && PY="$(command -v python3)"; } || install_uv_python
fi

# Private virtualenv so nothing touches the system Python.
if [ ! -x .venv/bin/python ]; then
    say "Creating virtual environment..."
    if ! "$PY" -m venv .venv 2>/dev/null; then
        if have apt-get; then $SUDO apt-get install -y python3-venv && "$PY" -m venv .venv
        else
            have uv || install_uv_python
            uv venv --python "$PY" .venv
        fi
    fi
fi

# Reinstall dependencies only when requirements.txt changes.
STAMP=.venv/.requirements.sha
SUM="$(sha256sum requirements.txt | cut -d' ' -f1)"
if [ "$(cat "$STAMP" 2>/dev/null)" != "$SUM" ]; then
    say "Installing dependencies..."
    if .venv/bin/python -m pip --version >/dev/null 2>&1; then
        .venv/bin/python -m pip install --quiet --upgrade pip
        .venv/bin/python -m pip install --quiet -r requirements.txt
    else
        uv pip install --python .venv/bin/python -r requirements.txt
    fi
    echo "$SUM" > "$STAMP"
fi

exec .venv/bin/python -m halfsword_lab "$@"

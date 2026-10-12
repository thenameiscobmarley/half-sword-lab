@echo off
rem Half Sword Lab launcher for Windows: gets Python + dependencies, then opens the terminal GUI.
rem   run.bat          normal mode
rem   run.bat --demo   try the GUI with fake data
setlocal
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=python"
)
if not defined PY (
    echo [half-sword-lab] Python 3.10+ not found, installing it with winget...
    winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements || goto :nopython
    set PY="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
)

if not exist .venv\Scripts\python.exe (
    echo [half-sword-lab] Creating virtual environment...
    %PY% -m venv .venv || goto :nopython
    .venv\Scripts\python -m pip install --quiet --upgrade pip
)
echo [half-sword-lab] Checking dependencies...
.venv\Scripts\python -m pip install --quiet -r requirements.txt
.venv\Scripts\python -m halfsword_lab %*
exit /b

:nopython
echo [half-sword-lab] Could not set up Python. Install it from https://www.python.org/downloads/ and run this again.
exit /b 1

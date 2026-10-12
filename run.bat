@echo off
rem Half Sword Lab launcher for Windows: checks what's missing, asks before installing anything, then opens the GUI.
rem   run.bat          normal mode
rem   run.bat --demo   try the GUI with fake data
setlocal
cd /d "%~dp0"

set "PY="
where py >nul 2>nul && py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=py -3"
if not defined PY (
    where python >nul 2>nul && python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>nul && set "PY=python"
)

set NEEDPY=0
set NEEDVENV=0
set NEEDDEPS=0
if not defined PY set NEEDPY=1
if not exist .venv\Scripts\python.exe set NEEDVENV=1
if not exist .venv\.deps-ok set NEEDDEPS=1

if "%NEEDPY%%NEEDVENV%%NEEDDEPS%" neq "000" (
    echo [half-sword-lab] Half Sword Lab needs to install:
    if %NEEDPY%==1 echo     - Python 3.12 with winget, from python.org
    if %NEEDVENV%==1 echo     - a private Python environment in %CD%\.venv
    if %NEEDDEPS%==1 echo     - Python packages from PyPI into .venv: textual
    choice /C YN /N /M "[half-sword-lab] Install these? [Y/N] "
    if errorlevel 2 (
        echo [half-sword-lab] Nothing installed. Bye.
        exit /b 0
    )
)

if not defined PY (
    winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements || goto :nopython
    set PY="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
)
if not exist .venv\Scripts\python.exe (
    %PY% -m venv .venv || goto :nopython
)
if not exist .venv\.deps-ok (
    .venv\Scripts\python -m pip install --quiet --upgrade pip
    .venv\Scripts\python -m pip install --quiet -r requirements.txt && echo ok> .venv\.deps-ok
)
.venv\Scripts\python -m halfsword_lab %*
exit /b

:nopython
echo [half-sword-lab] Could not set up Python. Install it from https://www.python.org/downloads/ and run this again.
exit /b 1

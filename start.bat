@echo off
setlocal
cd /d "%~dp0"
rem Python suchen: Store-Python 3.12, normale Installationen, py-Launcher, PATH
set "PY="
set "STOREPY=%LOCALAPPDATA%\Microsoft\WindowsApps\PythonSoftwareFoundation.Python.3.12_qbz5n2kfra8p0\python.exe"
if exist "%STOREPY%" set "PY=%STOREPY%"
set "PF86=%ProgramFiles(x86)%"
if not defined PY for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*" "%ProgramFiles%\Python3*" "%PF86%\Python3*") do if exist "%%~D\python.exe" set "PY=%%~D\python.exe"
if not defined PY if exist "%WINDIR%\py.exe" set "PY=%WINDIR%\py.exe"
if not defined PY set "PY=python"
echo Starte Launcher mit: %PY%
"%PY%" launcher.py
if errorlevel 1 pause

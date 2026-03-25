@echo off
setlocal

set PROJECT_DIR=%~dp0..
python "%PROJECT_DIR%\scripts\verify_backends.py" %*

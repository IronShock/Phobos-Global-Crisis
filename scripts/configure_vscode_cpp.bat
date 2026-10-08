@if not defined _echo echo off
python "%~dp0dev.py" setup-vscode
exit /b %errorlevel%

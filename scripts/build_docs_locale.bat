@if not defined _echo echo off
python "%~dp0dev.py" docs-locale
exit /b %errorlevel%

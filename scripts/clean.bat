@if not defined _echo echo off
python "%~dp0dev.py" clean
exit /b %errorlevel%

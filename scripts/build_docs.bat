@if not defined _echo echo off
python "%~dp0dev.py" docs
exit /b %errorlevel%

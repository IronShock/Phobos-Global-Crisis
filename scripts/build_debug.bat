@if not defined _echo echo off
rem Build Debug; additional MSBuild arguments may follow.
call "%~dp0run_msbuild.bat" /maxCpuCount /consoleloggerparameters:NoSummary /property:Configuration=Debug %*
exit /b %errorlevel%

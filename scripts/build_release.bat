@if not defined _echo echo off
rem Build Release; additional MSBuild arguments may follow.
call "%~dp0run_msbuild.bat" /maxCpuCount /consoleloggerparameters:NoSummary /property:Configuration=Release %*
exit /b %errorlevel%

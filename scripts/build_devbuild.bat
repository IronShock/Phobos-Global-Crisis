@if not defined _echo echo off
rem Build DevBuild; additional MSBuild arguments may follow.
call "%~dp0run_msbuild.bat" /maxCpuCount /consoleloggerparameters:NoSummary /property:Configuration=DevBuild %*
exit /b %errorlevel%

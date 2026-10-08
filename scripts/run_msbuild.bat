@if not defined _echo echo off
setlocal

pushd "%~dp0.." || exit /b 2
call "%~dp0run_vsdevcmd.bat"
if errorlevel 1 (
  popd
  exit /b 2
)
msbuild Phobos.vcxproj /property:Platform=Win32 %*
set "gc_build_result=%errorlevel%"
popd
exit /b %gc_build_result%

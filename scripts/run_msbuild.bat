@if not defined _echo echo off
setlocal

pushd "%~dp0.." || exit /b 2
if not defined GC_BUILD_TARGET set "GC_BUILD_TARGET=legacy"
if not defined GC_PYTHON set "GC_PYTHON=python"
set "gc_solution="
if "%GC_BUILD_TARGET%"=="legacy" set "gc_solution=%~dp0..\Phobos.sln"
if "%GC_BUILD_TARGET%"=="reference" (
  "%GC_PYTHON%" "%~dp0gc_reference.py" verify
  if errorlevel 1 goto failed
  set "gc_solution=%~dp0..\build\gc-reference\Phobos.sln"
)
if "%GC_BUILD_TARGET%"=="upstream" (
  "%GC_PYTHON%" "%~dp0dependencies.py" check --profile upstream
  if errorlevel 1 goto failed
  set "gc_solution=%~dp0..\.deps\upstream\phobos\Phobos.sln"
)
if not defined gc_solution (
  echo Unknown GC_BUILD_TARGET: %GC_BUILD_TARGET%
  goto failed
)
call "%~dp0run_vsdevcmd.bat"
if errorlevel 1 goto failed
rem The solution supplies SolutionDir and maps x86 to the project's Win32 platform.
msbuild "%gc_solution%" /property:Platform=x86 %*
set "gc_build_result=%errorlevel%"
popd
exit /b %gc_build_result%

:failed
popd
exit /b 2

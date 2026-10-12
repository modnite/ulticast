@echo off
rem Release build of Ulticast with a bounded parallelism, so a compile can't
rem monopolise every core of this machine (it also drives live video/calls).
rem Override the job count with:  set ULTICAST_JOBS=8  &&  build.cmd
setlocal
set "ROOT=%~dp0"
if "%ULTICAST_JOBS%"=="" (set "JOBS=4") else (set "JOBS=%ULTICAST_JOBS%")
echo Building Ulticast (Release) with %JOBS% parallel jobs...
cmake --build "%ROOT%build" --config Release --parallel %JOBS%
endlocal

@echo off
setlocal

call "%~dp0find-msys2.cmd"
if errorlevel 1 exit /b 1

pushd "%~dp0.." || exit /b 1
call "%SUNSHINE_MSYS2_SHELL%" -defterm -here -no-start -ucrt64 -c "exec bash scripts/build-client-deps.sh android \"$SUNSHINE_ANDROID_DEPS_PREFIX\""
set "EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %EXIT_CODE%

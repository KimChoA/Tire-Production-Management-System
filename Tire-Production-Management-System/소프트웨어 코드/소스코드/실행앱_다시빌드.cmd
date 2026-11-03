@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_app.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" (
    echo Deployment failed. Check the error message above.
) else (
    echo Deployment completed successfully.
)
pause
exit /b %RESULT%

@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

if /i "%~1"=="--build" (
    call build.bat
    if errorlevel 1 goto :fail
)

set "BIN=build\bin"
set "DIST=dist"
set "STAGE=%DIST%\stage"

for %%F in (capusy.exe) do (
    if not exist "%BIN%\%%F" (
        echo [package-release] Missing %BIN%\%%F. Run build.bat first or pass --build.
        goto :fail
    )
)

where 7z >nul 2>nul
if errorlevel 1 (
    echo [package-release] 7z.exe not found in PATH. Install 7-Zip to build capusy.7z and capusy.zip.
    goto :fail
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not exist "%DIST%" mkdir "%DIST%"
mkdir "%STAGE%"

copy /y "%BIN%\capusy.exe" "%STAGE%\capusy.exe" >nul || goto :fail
if exist "LICENSE" copy /y "LICENSE" "%STAGE%\LICENSE" >nul
if exist "licenses" xcopy /e /i /y /q "licenses" "%STAGE%\licenses" >nul

if exist "%DIST%\capusy.7z" del /f /q "%DIST%\capusy.7z"
if exist "%DIST%\capusy.zip" del /f /q "%DIST%\capusy.zip"

pushd "%STAGE%"
echo [package-release] Creating %DIST%\capusy.7z...
7z a -t7z -mx=9 -ms=on "..\capusy.7z" * >nul
if errorlevel 1 (
    popd
    goto :fail
)

echo [package-release] Creating %DIST%\capusy.zip...
7z a -tzip -mx=9 "..\capusy.zip" * >nul
if errorlevel 1 (
    popd
    goto :fail
)
popd

rmdir /s /q "%STAGE%"

echo.
echo [package-release] Done:
for %%A in ("%DIST%\capusy.7z" "%DIST%\capusy.zip") do (
    echo   %%~nxA  ^(%%~zA bytes^)
    for /f "skip=1 tokens=1" %%H in ('certutil -hashfile "%%~A" SHA256 ^| findstr /v "CertUtil"') do (
        echo     SHA256: %%H
    )
)

popd
endlocal
exit /b 0

:fail
if exist "%STAGE%" rmdir /s /q "%STAGE%"
popd
endlocal
exit /b 1

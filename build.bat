@echo off
setlocal
echo ========================================================
echo Building FuckPlaygroundGames (amd_ags_x64.dll)
echo ========================================================

where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] MSVC environment not found. Please run from a Developer Command Prompt.
        exit /b 1
    )
)

if not exist "bin" mkdir "bin"

cl /LD /O2 /W3 /std:c++20 /Isrc ^
    src\proxy_main.cpp ^
    src\proxy_log.cpp ^
    src\proxy_vram.cpp ^
    src\proxy_guard.cpp ^
    src\proxy_overlay.cpp ^
    dxgi.lib user32.lib gdi32.lib ^
    /Fe:bin\amd_ags_x64.dll ^
    /Fo:bin\

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Built bin\amd_ags_x64.dll successfully!
) else (
    echo.
    echo [ERROR] Build failed.
    exit /b %ERRORLEVEL%
)

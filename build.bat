@echo off
REM Build script for Notepad with FZF
REM Requires MSYS2 with MinGW64

echo Building Notepad with FZF...
echo.

REM Check if make is available
where make >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: 'make' not found in PATH
    echo Please install MSYS2 and add it to your PATH
    echo Example: C:\msys64\mingw64\bin
    pause
    exit /b 1
)

REM Run make
make %*

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo Build successful!
echo.
echo To run the application: notepad_fzf.exe
echo.
pause

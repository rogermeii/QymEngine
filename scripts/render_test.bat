@echo off
REM Automated render test: capture frame and analyze with RenderDoc CLI
REM Usage: scripts\render_test.bat

setlocal

set PROJECT_DIR=%~dp0..
set EDITOR_EXE=%PROJECT_DIR%\build3\editor\Debug\QymEditor.exe
set RENDERDOC_CMD="C:\Program Files\RenderDoc\renderdoccmd.exe"
set CAPTURE_INFO=%PROJECT_DIR%\capture_path.txt
set CAPTURES_DIR=%PROJECT_DIR%\captures

echo === QymEngine Render Test ===
echo.

REM Clean up
if exist "%CAPTURE_INFO%" del "%CAPTURE_INFO%"

REM Step 1: Run editor in capture-and-exit mode
echo [1/3] Launching editor with --capture-and-exit...
"%EDITOR_EXE%" --capture-and-exit "%CAPTURE_INFO%"
echo.

if not exist "%CAPTURE_INFO%" (
    echo FAIL: No capture produced. Is RenderDoc installed?
    exit /b 1
)

REM Read capture path
set /p CAPTURE_PATH=<"%CAPTURE_INFO%"
echo Capture file: %CAPTURE_PATH%

REM Check capture file exists and has size
if not exist "%CAPTURE_PATH%" (
    echo FAIL: Capture file not found
    exit /b 1
)

for %%A in ("%CAPTURE_PATH%") do set CAPTURE_SIZE=%%~zA
echo Capture size: %CAPTURE_SIZE% bytes

if %CAPTURE_SIZE% LSS 1000 (
    echo FAIL: Capture file too small, likely empty
    exit /b 1
)
echo PASS: Capture file valid
echo.

REM Step 2: Extract thumbnail
echo [2/3] Extracting thumbnail...
set THUMB_PATH=%CAPTURES_DIR%\thumb.jpg
%RENDERDOC_CMD% thumb --out="%THUMB_PATH%" --format=jpg "%CAPTURE_PATH%"

if not exist "%THUMB_PATH%" (
    echo FAIL: Could not extract thumbnail
    exit /b 1
)

for %%A in ("%THUMB_PATH%") do set THUMB_SIZE=%%~zA
echo Thumbnail size: %THUMB_SIZE% bytes

if %THUMB_SIZE% LSS 5000 (
    echo FAIL: Thumbnail too small, likely black/empty render
    exit /b 1
)
echo PASS: Thumbnail has content
echo.

REM Step 3: Summary
echo [3/3] Cleanup...
del "%CAPTURE_INFO%"

echo.
echo === RENDER TEST PASSED ===
echo Capture:   %CAPTURE_PATH%
echo Thumbnail: %THUMB_PATH%
exit /b 0

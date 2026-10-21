@echo off
rem ============================================================
rem 烧录 WS63 生产镜像（默认烧 COM14）
rem   用法:  burn.bat            烧到 COM14
rem          burn.bat 19         烧到 COM19
rem   参数是 COM 号数字，不要写 "COM" 前缀。
rem   镜像固定为 output\ws63-liteos-app_all.fwpkg。
rem   注意：不带 -bin 全参数直接跑 flasher 会默认烧 COM14，别在其它口上裸跑。
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "PORTNUM=%~1"
if "%PORTNUM%"=="" set "PORTNUM=14"

if not exist "%ROOT%output\ws63-liteos-app_all.fwpkg" (
  echo [ERROR] 找不到镜像: %ROOT%output\ws63-liteos-app_all.fwpkg
  exit /b 1
)

cd /d "%ROOT%tools\BurnTool"
BurnTool.exe -com:%PORTNUM% -bin:"%ROOT%output\ws63-liteos-app_all.fwpkg" -signalbaud:921600 -console -reset
echo BURN_EXIT_CODE=%ERRORLEVEL%

@echo off
setlocal
cd /d "%~dp0"
call create_bootbin.bat
if errorlevel 1 exit /b %errorlevel%
"G:\AMDDesignTools\2025.2\Vitis\bin\program_flash.bat" -f BOOT.bin -offset 0 -flash_type qspi-x4-single -fsbl zynq_fsbl.elf -verify
if errorlevel 1 exit /b %errorlevel%
echo QSPI flash programmed and verified successfully.

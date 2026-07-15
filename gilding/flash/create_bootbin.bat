@echo off
setlocal
cd /d "%~dp0"
copy /y "..\project_1.runs\impl_1\ad9744_dds_top.bit" "ad9744_dds_top.bit" >nul
if errorlevel 1 exit /b %errorlevel%
"G:\AMDDesignTools\2025.2\Vivado\bin\bootgen.bat" -image bootbin.bif -arch zynq -o BOOT.bin -w on
if errorlevel 1 exit /b %errorlevel%
echo BOOT.bin generated successfully.

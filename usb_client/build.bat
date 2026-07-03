@echo off
REM ==========================================================================
REM  Збірка moto_usb.exe — нативний GUI-клієнт (Tkinter), БЕЗ браузера.
REM  Потрібен лише Python 3.8+ на машині збірки.
REM ==========================================================================
setlocal
echo [1/2] Встановлення залежностей (pyserial, pyinstaller)...
python -m pip install --upgrade pyserial pyinstaller || goto :err

set ICON=
if exist icon.ico set ICON=--icon icon.ico --add-data "icon.ico;."

echo [2/2] Збірка одного .exe (без консольного вікна, з іконкою)...
pyinstaller --onefile --windowed --name moto_usb %ICON% moto_gui.py || goto :err

echo.
echo ГОТОВО:  dist\moto_usb.exe
echo (Іконку можна перегенерувати: pip install pillow ^&^& python make_icon.py)
echo (Альтернатива — веб-міст: build_bridge.bat)
goto :eof

:err
echo.
echo ПОМИЛКА збірки. Переконайтесь, що встановлено Python 3 і він у PATH.
exit /b 1

@echo off
REM ==========================================================================
REM  Збірка moto_usb.exe — нативний GUI-клієнт (Tkinter), БЕЗ браузера.
REM  Потрібен лише Python 3.8+ на машині збірки. У користувача .exe працює
REM  сам по собі: ні Python, ні браузер не потрібні.
REM ==========================================================================
setlocal
echo [1/2] Встановлення залежностей (pyserial, pyinstaller)...
python -m pip install --upgrade pyserial pyinstaller || goto :err

echo [2/2] Збірка одного .exe (без консольного вікна)...
pyinstaller --onefile --windowed --name moto_usb moto_gui.py || goto :err

echo.
echo ГОТОВО:  dist\moto_usb.exe
echo Скопіюйте dist\moto_usb.exe куди завгодно і запускайте подвійним кліком.
echo.
echo (Альтернатива — веб-міст у будь-якому браузері: build_bridge.bat)
goto :eof

:err
echo.
echo ПОМИЛКА збірки. Переконайтесь, що встановлено Python 3 і він у PATH.
exit /b 1

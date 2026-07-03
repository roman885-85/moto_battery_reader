@echo off
REM ==========================================================================
REM  Альтернатива GUI: веб-міст. Той самий client_usb.html у БУДЬ-ЯКОМУ браузері,
REM  але COM-порт відкриває нативно (pyserial) — Web Serial/Chrome не потрібні.
REM ==========================================================================
setlocal
echo [1/2] Встановлення залежностей...
python -m pip install --upgrade pyserial pyinstaller || goto :err
echo [2/2] Збірка moto_usb_bridge.exe (client_usb.html вкладено всередину)...
pyinstaller --onefile --name moto_usb_bridge ^
  --add-data "..\client_usb.html;." ^
  moto_bridge.py || goto :err
echo.
echo ГОТОВО:  dist\moto_usb_bridge.exe
goto :eof
:err
echo ПОМИЛКА збірки.
exit /b 1

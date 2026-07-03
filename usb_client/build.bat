@echo off
REM ==========================================================================
REM  Збірка moto_usb.exe — самодостатній USB-клієнт для Moto IMPRES Reader.
REM  Потрібен лише Python 3.8+ на машині збірки. У користувача .exe працює
REM  БЕЗ Python і БЕЗ Chrome (інтерфейс відкриється у браузері за замовч.).
REM ==========================================================================
setlocal
echo [1/2] Встановлення залежностей (pyserial, pyinstaller)...
python -m pip install --upgrade pyserial pyinstaller || goto :err

echo [2/2] Збірка одного .exe...
REM client_usb.html лежить у батьківській теці репозиторію -> вкладаємо всередину exe.
pyinstaller --onefile --name moto_usb ^
  --add-data "..\client_usb.html;." ^
  moto_bridge.py || goto :err

echo.
echo ГОТОВО:  dist\moto_usb.exe
echo Скопіюйте dist\moto_usb.exe куди завгодно і запускайте подвійним кліком.
goto :eof

:err
echo.
echo ПОМИЛКА збірки. Переконайтесь, що встановлено Python 3 і він у PATH.
exit /b 1

@echo off
REM ==========================================================================
REM  Збірка moto_usb.exe — нативний GUI-клієнт (Tkinter), БЕЗ браузера.
REM  Потрібен лише Python 3.8+ на машині збірки.
REM ==========================================================================
setlocal
REM  Клієнт складається з ДВОХ файлів: moto_gui.py (вікно) і moto_models.py
REM  (рішення про список моделей, винесене окремо, щоб його міг ганяти
REM  хостовий тест). PyInstaller підхоплює другий сам за імпортом — але якщо
REM  його просто забули скопіювати, збірка мовчки дасть .exe, який падає на
REM  старті. Тому питаємо ЗАРАЗ.
if not exist moto_models.py (
  echo ПОМИЛКА: поруч немає moto_models.py — скопіюйте ВСЮ теку usb_client.
  exit /b 1
)
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

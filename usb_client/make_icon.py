#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Генерує icon.ico (акумулятор + блискавка) для moto_usb.exe та вікна GUI.
Запуск: python make_icon.py   (потрібен Pillow: pip install pillow)"""
from PIL import Image, ImageDraw

def rounded(d, box, r, **kw):
    d.rounded_rectangle(box, radius=r, **kw)

def draw(sz=256):
    S = sz
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = S / 256.0
    def p(*xy): return [v * k for v in xy]
    # темна підкладка (тема застосунку)
    rounded(d, p(6, 6, 250, 250), 48 * k, fill=(18, 22, 28, 255), outline=(42, 50, 63, 255), width=max(1, int(3 * k)))
    GRN = (46, 204, 113, 255)
    GOLD = (212, 175, 55, 255)
    # корпус батареї (горизонтальний)
    bw = int(10 * k)
    rounded(d, p(40, 84, 196, 172), 16 * k, outline=GRN, width=bw)
    # плюсовий контакт
    rounded(d, p(196, 108, 214, 148), 6 * k, fill=GRN)
    # заряд-сегменти (легкий фон)
    rounded(d, p(56, 100, 180, 156), 8 * k, fill=(46, 204, 113, 40))
    # блискавка (золото)
    bolt = [(150, 92), (96, 140), (124, 140), (104, 182), (168, 124), (136, 124)]
    d.polygon(p(*[c for xy in bolt for c in xy]), fill=GOLD)
    return img

base = draw(256)
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
base.save("icon.ico", sizes=sizes)
base.save("icon.png")
print("icon.ico + icon.png створено:", sizes)

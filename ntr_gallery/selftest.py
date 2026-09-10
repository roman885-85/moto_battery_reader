#!/usr/bin/env python3
"""Проверка index.html: разбор ZIP, каталог, сборка нового бэкапа.

НАВЕРНОЕ САМОЕ ВАЖНОЕ. Инструмент переписывает файл сохранения. Если ZIP
собран криво — игра молча откажется его импортировать, а исходный бэкап у
человека уже может быть затёрт. Поэтому здесь не «синтаксис цел», а полный
круг: взять настоящий бэкап, прогнать его через тот же код, что и в браузере,
и убедиться, что результат читается штатным zipfile и содержит ровно то, что
обещано.

Скрипт вытаскивает встроенный <script> из index.html и запускает его в node
(v18+, нужны CompressionStream/DecompressionStream). document там нет —
страница это учитывает и в таком случае не трогает интерфейс.

Запуск (из этой папки или из корня репозитория):
    python3 ntr_gallery/selftest.py [путь/к/бэкапу.ntrbckp]

Без аргумента собирает синтетический бэкап того же вида, что и настоящий.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "index.html")

SAMPLE_GALLERY = [
    {"character": "rachel", "filepath": "res://assets/images/rachel/Rachel01.jpg",
     "timestamp": 1778672862.6, "type": 0},
    {"character": "rachel", "filepath": "res://assets/images/rachel/Rachel03.jpg",
     "timestamp": 1778672862.6, "type": 0},
    {"character": "amber", "filepath": "res://assets/audio/amber/Amber_Ch_2_A1.mp3",
     "timestamp": 1778672862.6, "type": 1},
]
SAMPLE_CONTACTS = [
    {"name": "rachel", "state": "unlocked"},
    {"name": "blake", "state": "locked"},
    {"name": "playground", "state": "dev-locked"},
]


def make_sample(path):
    """Синтетический бэкап: те же имена файлов и то же сжатие, что у игры."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("save_info", json.dumps(
            {"version": "31", "build_version": "0.45.0 [test]"}, indent="\t"))
        z.writestr("virtualphone/messenger/", b"")
        z.writestr("virtualphone/messenger/contacts_list",
                   json.dumps(SAMPLE_CONTACTS, separators=(",", ":")))
        z.writestr("virtualphone/gallery/", b"")
        z.writestr("virtualphone/gallery/unlocked_photos",
                   json.dumps(SAMPLE_GALLERY, separators=(",", ":")))
        z.writestr("virtualphone/tracker/completion_cache",
                   json.dumps({"rachel": 12, "amber": 3}, indent="\t"))


def page_script():
    text = open(PAGE, encoding="utf-8").read()
    chunks = re.findall(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", text, re.S | re.I)
    if not chunks:
        raise SystemExit("в index.html нет встроенного <script>")
    return "\n;\n".join(chunks)


HARNESS = r"""
/* --- проверки поверх кода страницы ------------------------------------- */
const fs = require('node:fs');
const SRC = process.argv[2], OUT = process.argv[3];

let fails = 0;
function ok(cond, what, extra) {
  console.log((cond ? '  ок   ' : '  ПЛОХО') + '  ' + what + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
}
const toArrayBuffer = u8 => u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
const jsonOf = (entries, name) =>
  JSON.parse(new TextDecoder().decode(entries.find(e => e.name === name).data));

(async () => {
  const raw = new Uint8Array(fs.readFileSync(SRC));
  const entries = await zipRead(toArrayBuffer(raw));
  console.log('\nЧтение бэкапа');
  ok(entries.length > 0, 'записей в архиве', entries.length);
  const gallery = jsonOf(entries, F_GALLERY);
  ok(Array.isArray(gallery), 'галерея разобрана', gallery.length + ' записей');

  console.log('\nКаталог 0.45.0');
  const cat = expandCatalog(CATALOG_0450);
  ok(cat.length === 690, 'записей в каталоге', cat.length);
  ok(new Set(cat.map(e => e.filepath)).size === cat.length, 'дублей путей нет');
  ok(cat.every(e => /^res:\/\/assets\/(images|audio|video)\/[^/]+\/.+\.[a-z0-9]+$/.test(e.filepath)),
     'все пути выглядят как res://assets/...');
  ok(cat.some(e => e.filepath.endsWith('/Rachel63.jpg')), 'дырка Rachel63 закрыта');
  ok(cat.some(e => e.filepath.endsWith('/Amber22.jpg')), 'дырка Amber22 закрыта');
  ok(cat.filter(e => e.filepath.endsWith('/Cathryn60.jpg'))[0].character === 'alice',
     'Cathryn60 остался за alice');
  const paths0 = new Set(cat.map(e => e.filepath));
  ok(paths0.has('res://assets/images/rachel/Rachel01.jpg') &&
     !paths0.has('res://assets/images/rachel/Rachel1.jpg'),
     'нули впереди сохранены: Rachel01, а не Rachel1');
  ok(paths0.has('res://assets/images/rachel/Rachel102.jpg'),
     'та же серия без нулей там, где номер длинный: Rachel102');
  ok(paths0.has('res://assets/video/amber/amber_animation_01.ogv'),
     'видео тоже с нулями: amber_animation_01');
  const build045 = (jsonOf(entries, F_INFO).build_version || '').startsWith('0.45');
  if (build045)
    ok(gallery.every(e => paths0.has(e.filepath)),
       'каталог покрывает всё, что уже открыто в этом бэкапе',
       gallery.filter(e => !paths0.has(e.filepath)).slice(0, 3).map(e => e.filepath).join(' '));

  console.log('\nПлан разблокировки');
  const added = planUnlock(gallery, cat);
  const have = new Set(gallery.map(e => e.filepath));
  ok(added.every(e => !have.has(e.filepath)), 'ничего уже открытого в добавке нет');
  ok(new Set(added.map(e => e.filepath)).size === added.length, 'дублей в добавке нет');
  ok(planUnlock(gallery.concat(added), cat).length === 0, 'повторный проход ничего не добавляет');
  console.log('       добавится ' + added.length + ', станет ' + (gallery.length + added.length));

  console.log('\nПродолжение серий');
  const ext5 = extendSeries(cat, 5);
  ok(ext5.length === seriesOf(cat).length * 5, 'по 5 номеров на каждую серию', ext5.length);
  ok(ext5.every(e => !cat.some(c => c.filepath === e.filepath)), 'продолжение не пересекается с каталогом');
  ok(extendSeries(cat, 0).length === 0, 'ноль означает ноль');
  const extPaths = new Set(ext5.map(e => e.filepath));
  ok(extPaths.has('res://assets/video/rachel/rachel_animation_03.ogv'),
     'продолжение держит нули впереди');
  ok(extPaths.has('res://assets/images/rachel/Rachel103.jpg'),
     'продолжение длинных номеров без лишних нулей');

  console.log('\nОглавление .pck');
  const mkPck = (list, ver) => {
    const enc = new TextEncoder();
    const items = list.map(s => {
      const b = enc.encode(s);
      return { b, pad: (4 - b.length % 4) % 4 };
    });
    const entrySize = it => 4 + it.b.length + it.pad + 8 + 8 + 16 + (ver >= 2 ? 4 : 0);
    let size = 20 + (ver >= 2 ? 12 : 0) + 64 + 4 + items.reduce((a, it) => a + entrySize(it), 0);
    const u8 = new Uint8Array(size), dv = new DataView(u8.buffer);
    let p = 0;
    dv.setUint32(p, 0x43504447, true); p += 4;
    dv.setUint32(p, ver, true); p += 4;
    p += 12;                                       // мажор / минор / патч
    if (ver >= 2) { p += 4; dv.setBigUint64(p, 0n, true); p += 8; }
    p += 64;
    dv.setUint32(p, items.length, true); p += 4;
    for (const it of items) {
      dv.setUint32(p, it.b.length + it.pad, true); p += 4;
      u8.set(it.b, p); p += it.b.length + it.pad;
      p += 8 + 8 + 16 + (ver >= 2 ? 4 : 0);
    }
    return u8;
  };
  const want = ['res://assets/images/rachel/Rachel01.jpg', 'res://assets/audio/amber/Amber_Ch_2_A1.mp3'];
  for (const ver of [1, 2]) {
    const got = pckIndex(toArrayBuffer(mkPck(want, ver)));
    ok(got.length === want.length && got.every((s, i) => s === want[i]),
       'формат пака версии ' + ver, JSON.stringify(got));
  }
  const pck = mkPck(want, 2);
  const exe = new Uint8Array(64 + pck.length + 12);
  exe.set(pck, 64);
  new DataView(exe.buffer).setBigUint64(64 + pck.length, BigInt(pck.length), true);
  new DataView(exe.buffer).setUint32(64 + pck.length + 8, 0x43504447, true);
  ok(pckIndex(toArrayBuffer(exe)).length === want.length, 'пак, вшитый в .exe');
  let threw = '';
  try { pckIndex(toArrayBuffer(new Uint8Array(40))); } catch (e) { threw = e.message; }
  ok(threw !== '', 'мусор вместо пака — понятная ошибка', threw);

  console.log('\nСписок из путей игры (как из .pck)');
  const paths = cat.map(e => e.filepath + '.import')
    .concat(['res://assets/dialogue/rachel/x.dialogue', 'res://.godot/imported/Rachel1.jpg-ab.ctex',
             'res://assets/images/rachel/Rachel999.jpg']);
  const man = manifestFromPaths(paths, gallery);
  ok(man.length === cat.length + 1, 'мусор отсеян, .import снят', man.length);
  ok(man.find(e => e.filepath.endsWith('Rachel999.jpg')).character === 'rachel',
     'хозяин нового файла угадан по соседям');
  ok(man.find(e => e.filepath.endsWith('Rachel999.jpg')).type === 0, 'тип по расширению');
  ok(man.find(e => e.filepath.endsWith('Amber_Ch_2_A1.mp3')).type === 1, 'mp3 это аудио');

  console.log('\nЗапись бэкапа');
  const now = 1789000000.5;
  const merged = gallery.concat(added.map(e => ({
    character: e.character, filepath: e.filepath, timestamp: now, type: e.type })));
  writeJSON(entries, F_GALLERY, merged);
  const contacts = jsonOf(entries, F_CONTACTS);
  for (const c of contacts) if (c.state === 'locked') c.state = 'unlocked';
  writeJSON(entries, F_CONTACTS, contacts);
  const blob = await zipWrite(entries);
  const out = new Uint8Array(await blob.arrayBuffer());
  fs.writeFileSync(OUT, out);
  ok(out.length > 0, 'архив собран', out.length + ' байт');

  const back = await zipRead(toArrayBuffer(out));
  ok(back.length === entries.length, 'число записей сохранилось');
  ok(back.every((e, i) => e.name === entries[i].name), 'имена и порядок записей сохранились');
  ok(back.every((e, i) => e.data.length === entries[i].data.length), 'размеры совпали');
  const back2 = jsonOf(back, F_GALLERY);
  ok(back2.length === merged.length, 'галерея вернулась целиком', back2.length);
  ok(back2.every(e => 'character' in e && 'filepath' in e && 'timestamp' in e && 'type' in e),
     'у всех записей четыре поля игры');
  ok(jsonOf(back, F_CONTACTS).every(c => c.state !== 'locked'), 'закрытых контактов не осталось');

  process.exit(fails ? 1 : 0);
})().catch(e => { console.error('\nсорвалось: ' + (e.stack || e)); process.exit(2); });
"""


def main():
    node = shutil.which("node") or shutil.which("nodejs")
    if not node:
        print("node не найден — проверку пропущено (это не «пройдено»)")
        return 0

    tmp = tempfile.mkdtemp(prefix="ntrgal-")
    try:
        src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tmp, "sample.ntrbckp")
        if len(sys.argv) > 1:
            print("бэкап: %s" % src)
        else:
            make_sample(src)
            print("бэкап: синтетический (аргументом можно дать настоящий .ntrbckp)")

        js = os.path.join(tmp, "page.js")
        with open(js, "w", encoding="utf-8") as f:
            f.write(page_script())
            f.write(HARNESS)

        out = os.path.join(tmp, "out.ntrbckp")
        r = subprocess.run([node, js, src, out], text=True)
        if r.returncode == 0:
            with zipfile.ZipFile(out) as z:
                bad = z.testzip()
            print("\n  ок     штатный zipfile принял архив" if bad is None
                  else "\n  ПЛОХО  zipfile ругается на " + bad)
            if bad is not None:
                return 1
            print("\nвсё сходится")
        else:
            print("\nЕСТЬ ОШИБКИ")
        return r.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

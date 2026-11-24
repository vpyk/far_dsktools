# DskTools

[Русский](#русский) · [English](#english)

## Русский

Плагин для [FAR Manager 3](https://www.farmanager.com/), открывающий образы дискет ретро-компьютеров как обычную панель: каталоги, просмотр, копирование файлов наружу и — для сырых образов — запись, переименование и удаление внутри образа. Работает на библиотеке [dsk_tools](https://github.com/Ptr314/dsk_tools), на которой построен [DISK Commander](https://github.com/Ptr314/dsk_commander).

### Что открывается

| Компьютеры | Файловые системы | Запись |
|---|---|---|
| Агат, Apple II | DOS 3.3, ProDOS, Sprite OS, Onix, CP/M | DOS 3.3, CP/M |
| Корвет, Орион-128, ПК8000, Вектор-06Ц, Ириша, ГМД-7012 | CP/M | да |
| IBM PC, Atari ST | FAT | да |
| ДВК, БК, УКНЦ | RT-11 | да |
| Искра-226 | своя | нет |

Форматы файлов: сырые образы секторов (`.dsk`, `.do`, `.po`, `.cpm`, `.kdi`, `.odi`, `.fdd`, `.gmd`, `.img`, `.ima`, `.st`, `.bkd`, `.rtd`), HFE, MFM/NIB/NIC, AIM, IMD, отдельные файлы FIL и SPR.

Записывать можно только в **сырые образы** — файл образа сохраняется сразу после каждой операции. HFE, MFM/NIB/NIC, AIM, IMD и образы с заголовком открываются только для чтения: их запись перекодировала бы все дорожки. Если запись запрещена, в заголовке панели стоит `[R/O]`, причина — в информационной панели (Ctrl+L).

Для CP/M плагин сам находит геометрию диска: по таблице параметров в загрузочном секторе (Орион, Корвет, Вектор) или подбором по каталогу, если по расширению и размеру файла она определяется неверно. Открываются и образы с обрезанным пустым концом; при записи файл растёт только до записанных данных. Области пользователя CP/M 1–31 показываются папками `USER_01`…`USER_31`.

### Внутренняя работа с образами

Описание внутренней работы с образами находится в [IMAGE_HANDLING.md](IMAGE_HANDLING.md).

### Клавиши

| Действие | Клавиши |
|---|---|
| Открыть образ под курсором | Enter, Ctrl+PgDn |
| Открыть образ из командной строки | `dsk:<файл>` |
| Выйти из образа (в корне) | Enter на `..`, Ctrl+PgUp |
| Копировать, перенести в другую панель или в образ | F5, F6 |
| Копия под другим именем в той же папке | Shift+F5 |
| Переименовать | Shift+F6 |
| CP/M: перенести в другую область пользователя | Alt+F6 |
| Создать папку (CP/M: область пользователя) | F7 |
| Удалить | F8 |
| Просмотр, редактирование | F3, F4 |
| Информация об образе | Ctrl+L |
| Поиск файлов | Alt+F7 |

F6 между двумя панелями одного образа CP/M переносит файлы в область другой панели, не копируя данные. Справка — F1 на панели плагина.

### Настройки

F9 → Параметры → Параметры внешних модулей → DskTools (или Alt+Shift+F9 на панели плагина):

- открывать образы по Enter и Ctrl+PgDn, маска файлов, файловые системы, которые плагин забирает себе (остальные FAR отдаёт другим плагинам);
- разрешить запись в образы;
- резервная копия `<образ>.bak` перед первым изменением образа.

### Установка

1. Скачайте архив нужной разрядности: x64 — для 64-битного FAR, x86 — для 32-битного (DLL другой разрядности FAR молча не загрузит).
2. Распакуйте папку `DskTools` в `Plugins` каталога FAR — внутри должны лежать `DskTools.dll`, `DskToolsEng.lng`, `DskToolsRus.lng`, `DskToolsEng.hlf`, `DskToolsRus.hlf`.
3. Перезапустите FAR.

Требования: FAR Manager 3.0 сборки 3800 или новее; [Microsoft Visual C++ Redistributable 2015–2022](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) версии 14.44 или новее той же разрядности, что и плагин. Проверено на Windows 10.

### Сборка из исходников

Нужны MSVC 2022 Build Tools и CMake 3.25+.

```
git clone --recurse-submodules <адрес репозитория>
cmake --preset x64 && cmake --build --preset x64
cmake --preset x86 && cmake --build --preset x86
```

Результат — в `build/<x64|x86>/Release/`. Архив для установки (папка `DskTools`, `README.md`, `LICENSE`) собирается целью `dist`:

```
cmake --build --preset x64 --target dist
cmake --build --preset x86 --target dist
```

и лежит в `build/<x64|x86>/DskTools-<версия>-<x64|x86>.zip`.

### Лицензия

DskTools распространяется на условиях [GNU GPL версии 3 или более поздней](LICENSE). © 2026 Viktor Pykhonin.

В плагине использованы:

- [dsk_tools](https://github.com/Ptr314/dsk_tools) — © Mikhail Revzin, GPL-3.0-or-later (линкуется статически);
- заголовки [FAR Manager Plugin SDK](https://github.com/FarGroup/FarManager) — © Eugene Roshal, Far Group, лицензия BSD с исключением для плагинов (`third_party/FarPluginSDK`);
- тексты информационной панели из [DISK Commander](https://github.com/Ptr314/dsk_commander) — GPL-3.0.

## English

A [FAR Manager 3](https://www.farmanager.com/) plugin that opens disk images of retro computers as an ordinary panel: folders, viewing, copying files out and — for raw images — writing, renaming and deleting inside the image. Built on the [dsk_tools](https://github.com/Ptr314/dsk_tools) library, which [DISK Commander](https://github.com/Ptr314/dsk_commander) is built on as well.

### What it opens

| Computers | Filesystems | Writing |
|---|---|---|
| Agat, Apple II | DOS 3.3, ProDOS, Sprite OS, Onix, CP/M | DOS 3.3, CP/M |
| Korvet, Orion-128, PK8000, Vector-06C, Irisha, GMD-7012 | CP/M | yes |
| IBM PC, Atari ST | FAT | yes |
| DVK, BK, UKNC | RT-11 | yes |
| Iskra-226 | its own | no |

File formats: raw sector images (`.dsk`, `.do`, `.po`, `.cpm`, `.kdi`, `.odi`, `.fdd`, `.gmd`, `.img`, `.ima`, `.st`, `.bkd`, `.rtd`), HFE, MFM/NIB/NIC, AIM, IMD, single FIL and SPR files.

Only **raw images** can be written — the image file is saved right after every operation. HFE, MFM/NIB/NIC, AIM, IMD and images with a header open read-only: writing them would re-encode every track. When writing is refused the panel title ends with `[R/O]`, and the info panel (Ctrl+L) tells why.

For CP/M the plugin finds the disk geometry itself: from the parameter block in the boot sector (Orion, Korvet, Vector) or by trying layouts on the directory, where the file's extension and size alone would pick a wrong one. Images with the empty end cut off open too; writing grows the file only as far as the data. CP/M user areas 1–31 are shown as folders `USER_01`…`USER_31`.

### Internal image-handling details

See [IMAGE_HANDLING.md](IMAGE_HANDLING.md) for the internal image-handling details.

### Keys

| Action | Keys |
|---|---|
| Open the disk image under the cursor | Enter, Ctrl+PgDn |
| Open an image from the command line | `dsk:<file>` |
| Close the image (at its root) | Enter on `..`, Ctrl+PgUp |
| Copy, move to the other panel or into the image | F5, F6 |
| Copy under another name in the same folder | Shift+F5 |
| Rename | Shift+F6 |
| CP/M: move into another user area | Alt+F6 |
| Make a folder (CP/M: a user area) | F7 |
| Delete | F8 |
| View, edit | F3, F4 |
| Information about the image | Ctrl+L |
| Find files | Alt+F7 |

F6 between two panels of one CP/M image moves the files into the other panel's user area without copying their data. Help: F1 on the plugin's panel.

### Settings

F9 → Options → Plugins configuration → DskTools (or Alt+Shift+F9 on the plugin's panel):

- open images on Enter and Ctrl+PgDn, a file mask, the filesystems the plugin takes (FAR hands the others to other plugins);
- allow writing into images;
- a backup `<image>.bak` before the first change of an image.

### Installation

1. Download the archive of the right bitness: x64 for 64-bit FAR, x86 for 32-bit FAR (FAR silently skips a DLL of the other bitness).
2. Unpack the `DskTools` folder into FAR's `Plugins` folder — it holds `DskTools.dll`, `DskToolsEng.lng`, `DskToolsRus.lng`, `DskToolsEng.hlf`, `DskToolsRus.hlf`.
3. Restart FAR.

Requirements: FAR Manager 3.0 build 3800 or newer; the [Microsoft Visual C++ Redistributable 2015–2022](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist), version 14.44 or newer, of the plugin's bitness. Tested on Windows 10.

### Building from source

Requires MSVC 2022 Build Tools and CMake 3.25+.

```
git clone --recurse-submodules <repository URL>
cmake --preset x64 && cmake --build --preset x64
cmake --preset x86 && cmake --build --preset x86
```

The result is in `build/<x64|x86>/Release/`. The archive to install from (the `DskTools` folder, `README.md`, `LICENSE`) is built by the `dist` target:

```
cmake --build --preset x64 --target dist
cmake --build --preset x86 --target dist
```

and lands in `build/<x64|x86>/DskTools-<version>-<x64|x86>.zip`.

### License

DskTools is distributed under the [GNU GPL version 3 or later](LICENSE). © 2026 Viktor Pykhonin.

It uses:

- [dsk_tools](https://github.com/Ptr314/dsk_tools) — © Mikhail Revzin, GPL-3.0-or-later (linked statically);
- the [FAR Manager Plugin SDK](https://github.com/FarGroup/FarManager) headers — © Eugene Roshal, Far Group, a BSD license with an exception for plugins (`third_party/FarPluginSDK`);
- info panel texts from [DISK Commander](https://github.com/Ptr314/dsk_commander) — GPL-3.0.

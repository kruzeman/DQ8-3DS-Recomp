# План развития DQ8 — статический рекомпилятор Dragon Quest VIII (3DS, ARM11 → x86_64/C11/Rust)

> Аналитический план, составленный на основе фактического аудита исходников,
> запуска тестов и работы инструментария. Язык документа — русский (в соответствии
> с требованиями задачи). Документ основан на реальном выводе терминала, а не на
> допущениях; раздел «Примечания об окружении и достоверности» описывает, что
> удалось проверить, а что заблокировано отсутствием тулчейна.
>
> Статус на 2026-10-20: тулчейн **установлен и проверен** — P0.1 закрыта.
> `dq8tool` пересобран из текущих исходников; все 6 нативных наборов и все 35
> unit‑теста (`cargo test`) зелёные. Главный оставшийся блокер — **игра не
> достигает видимого титульного кадра** (см. §4).

---

## 0. Краткая сводка (Executive summary)

Проект — это самодостаточный (нулевых внешних Rust-зависимостей) CLI `dq8tool`,
который дизассемблирует ARM11/Thumb-код из `DQ8.code`, эмулирует SVC и 3DS-сервисы,
и рекомпилирует игру в один большой C-файл (`target/dq8-startup.c`), в который
встраиваются три C-рунтайма (`pica_vs.c`, `native_fs.c`, `native_hid.c`) через
`include_str!`. Полученный C компилируется `clang` + SDL2/OpenGL и запускается как
двухэкранное окно 3DS.

Текущий статус запуска (по `docs/*` и `README.md`): игра проходит инициализацию
heap, RomFS, DSP/CFG, регистрирует GSP-очередь, разбирает PICA-регистры, рендерит
стартовые логотипы и выходит на диалог создания/загрузки сохранения после логотипов
(`docs/postlogo-save-prompt.md`). Далее — без видимого титульного кадра: захваченные
PICA-списки рисуют нулюадраты (DMA fallback), текстуры не привязаны, framebuffer
остаются чёрными, а путь `title002_u.bflyt` застревает на `Visible_` lookup
(`README.md` L34, L127–131).

**Главный блокер среды (РЕШЁН, P0.1 закрыта):** тулчейн `cargo`/`rustc`/`clang`/
`ctrtool`/`sdl2` **установлен и проверен** на хосте (см. §3). README L112 заявлял
«Rust and Cargo are available locally», и это теперь **соответствует действительности**.
`dq8tool` пересобран из текущих исходников `cargo build --release` (бинарь 04:14
2026-10-20, позже HEAD `ec16f8c` 19:38:59). Все 6 нативных тестовых наборов и все
35 unit‑теста (`cargo test`) проходят чисто (см. §6). Ранее «4/6 падений» были
искусственно вызваны **устаревшим бинарём** (19:08), а не реальными багами исходника.

---

## 1. Статистика кодовой базы (факты из терминала)

| Артефакт | Путь | Строк/Размер | Статус git | Примечание |
|---|---|---|---|---|
| Рекомпилятор + CLI | `src/main.rs` | 8058 строк | затр. | ARM→C, SVC, планировщик, 34 unit-теста |
| PICA200 вершинный шейдер | `src/pica_vs.c` | 359 строк | **закоммичен** | эмуляция регистров/Uniform/swzizzle |
| FS runtime | `src/native_fs.c` | 147 строк | **НЕ закоммичен** (untracked) | RomFS/SaveData/SDMC |
| HID runtime | `src/native_hid.c` | 46 строк | **НЕ закоммичен** (untracked) | кольцевой буфер HID |
| Точки входа рекомпиляции | `recomp-roots.txt` | 1236 корней | **НЕ закоммичен** (+115 в рабочем дереве) | — |
| Noreturn точки | `recomp-noreturn.txt` | 1 корень (`0x0017B780`) | закоммичен | contexts restore, не возвращает |
| Тест-утилиты | `tools/test_*.py` | 6 файлов | 3 закомм., 3 untracked | см. ниже |
| `DQ8.code` | корень | 5 218 304 Б | в .gitignore | BLZ-распакованный .text (0x43BCB4) |
| README | корень | 146 строк / 15 980 симв. | закоммичен | архитектура + статус |

Подтверждено `git ls-files`: из C-рунтаймов в git попал **только** `pica_vs.c`.
`native_fs.c` и `native_hid.c` существуют только на диске. То же касается
`test_native_fs.py`, `test_native_hid.py`, `test_pica_texture.py`.

---

## 2. Архитектурный конвейер (цепь трансляции)

Источник: `README.md` L14–48, `tools/run_static_recomp.sh`.

1. `DQ8.code` — расшифрованный (через `ctrtool`, либо `decrypt-code`/`extract-code`)
   и BLZ-распакованный ARM11/Thumb-образ (загрузка по `0x00100000`, text `0x43BCB4`).
2. `dq8tool recompile-arm DQ8.code 0x00100000 target/dq8-startup.c
   --extra-entry <recomp-roots.txt> --noreturn-call <recomp-noreturn.txt>`
   → C11 с PC-диспетчером, гостевыми регистрами/флагами, memory ops,условными ветвями,
   VFP, блок-переносами и шиммами SVC/сервисов. Инъецирует `pica_vs.c`/`native_fs.c`/
   `native_hid.c` через `include_str!` (main.rs:6981–6983). Последний запуск:
   181 600 cases / 17 003 функций / 16.1 % диапазона `.text` (README L34).
3. `clang -std=c11 [-O2|O0] target/dq8-startup.c -lm [-lSDL2] -o target/dq8-startup`
   — `run_static_recomp.sh` (строка 40). SDL2 подключается через `sdl2-config`.
4. `target/dq8-startup DQ8.code target/dq8.romfs` — запуск с двумя экранами SDL2
   (800×960: топ 400×240 / низ 320×240). `DQ8_HEADLESS=1` — без окна.

Вспомогательные подсистемы `dq8tool`: `inspect` (NCSD/NCCH), `disasm` (ARM/Thumb→pседокод),
`decompile` (BFS по ветвям/BLX, 379 косвенных таргетов, `LDR pc` веникеры →
`DQ8.decomp.indirect.md` + `DQ8.decomp.c`), `recompile-entry` (proof-of-concept),
`extract-sarc`, `decrypt-code`. Есть отдельный рантер `dq8-title` — только
декодер BFLIM/BFLYT, **не** рекомпилятор (README L86–102).

---

## 3. Примечания об окружении и достоверности проверок

Все утверждения ниже подтверждены реальным запуском в терминале (20 октября 2026 г.,
эта сессия). Тулчейн, который в предыдущей сессии считался **отсутствующим**, теперь
**есть на хосте** (установлен Ops):

- `cargo 1.99.1`, `rustc 1.99.1` — `/home/hermes/.cargo/bin/` ✓
- `clang 18.1.3` (`/usr/bin/clang`) ✓, `gcc 13.3.0` (`/usr/bin/gcc`) ✓
- `ctrtool` (`/usr/local/bin/ctrtool`) ✓; `sdl2-config 2.30.0` ✓ (`libsdl2-dev` установлен)
- Бинарь `target/release/dq8tool` **пересобран**: `cargo build --release` → exit 0,
  8.39 с, размер 1 057 120 Б (новее HEAD `ec16f8c` 19:38:59). Больше нет stale-binary.

**Проверено фактически (зелёное по пересобранному бинарю):**

- `cargo test` → **35/35** (34 из `main.rs` + 1 из `src/bin/dq8-title.rs`): `ok`.
- `disasm DQ8.code --mode arm --base 0x00100000 --count 16` → корректный псевдокод
  (`call`, `goto`, `branch(lr)`, загрузка регистров).
- `recompile-arm stub.code 0x00100000 stub.c` → C с шаблонами; `clang -std=c11 -O0 -w`
  и `gcc -std=c11 -O0 -w` компилируют его (exit 0 оба).
- **Полный конвейер** `recompile-arm DQ8.code 0x00100000 … (1236 корней)`:
  259 231 инструкций / 24 206 функций → 264 596 строк `dq8-startup.c`, компилируется
  и линкуется `clang` + SDL2 **и** в headless‑режиме (`DQ8_HEADLESS=1`) — exit 0.
- **Все 6 нативных наборов** `tools/test_*.py` проходят чисто против пересобранного
  бинаря (см. таблицу §6).

> Вывод предыдущей сессии о «4/6 падениях из‑за устаревшего бинаря» **подтверждён
> и устранен**: после пересборки из исходников `ec16f8c` + рабочего дерева все
> наборы зелёные. Никаких регрессий в текущем исходнике не выявлено.

---

## 4. Текущий статус запуска

Источники: `docs/progress-2026-10-19.md`, `docs/apt-home-review-2026-10-19.md`,
`docs/runtime-wait-fixes.md`, `docs/gsp-overlay-stall.md`,
`docs/guest-startup-logo.md`, `docs/postlogo-save-prompt.md`, `README.md` L34.

- [x] heap setup, чтение заголовка и метаданных RomFS, загрузка больших RomFS-ассетов.
- [x] шимы DSP и CFG на старте; регистрация GSP relay queue; события PDC0/VBlank;
  разбор PICA-регистров в runtime state.
- [x] стартовые логотипы Square Enix / Armor Project / Bird Studio рендерятся
  (`c41bf0d`, `98cb423`).
- [x] переход к диалогу создания/загрузки сохранения после логотипов
  (`postlogo-save-prompt.md`).
- [x] поправки APT (state leakage → спурьные HOME, `f4a12dd`),
  `svcWaitSynchronizationN` timeout-ABI + main joins (`6d58309`),
  мульти‑объект wait / arbiter Signal / file-handle collisions (`ec16f8c`).
- [ ] **видимый титульный кадр**: захваты PICA рисуют нулюадраты, texture0 не
  привязана, framebuffer чёрные; путь `title002_u.bflyt` застревает на `Visible_`
  lookup. Игра ещё не открыла интро‑фильм и не запросила savedata (README L34).

---

## 5. Технический долг и незавершённые подсистемы

### 5.1 Не реализованные SVC / сервисы (`src/main.rs`)
10 обработчиков пишут `unimplemented ...` (и в некоторых `exit()`):

| № | Адрес строки | SVC / сервис | Поведение |
|---|---|---|---|
| 1 | 5021 | `SRV:GetServiceHandle` | unimplemented |
| 2 | 5817 | `SVC SendSyncRequest` | unimplemented |
| 3 | 5840 | `SVC ConnectToPort` | unimplemented |
| 4 | 5862 | `SVC CloseHandle` | unimplemented |
| 5 | 6154 | `SVC GetProcessId` | unimplemented |
| 6 | 6220 | `SVC ReleaseMutex` | unimplemented |
| 7 | 6249 | `SVC SignalEvent` | unimplemented → `exit(23)` |
| 8 | 6259 | `SVC ClearEvent` | unimplemented → `exit(24)` |
| 9 | 6481 | `SVC UnmapMemoryBlock` | unimplemented |
| 10 | 6496 | `SVC MapMemoryBlock` | unimplemented |

Отметим, что реализованы и работают (подтверждены тестами): `svc_create_event`
(2777), `svc_create_address_arbiter` (2783), `svc_arbitrate` (2800),
`svc_wait_synchronization_n` (6407/6446/6470). Адресаты `SignalEvent`/`ClearEvent`
жёстко `exit()`‑ят — из‑за этого некоторые игровые пути будущего теряются.

### 5.2 PICA200 / графика
- Цветовой адрес `@0x11D` и кодировка размеров `@0x11E` до сих пор «не сопоставлена»
  (main.rs:3728–3732): ширина/высота цветового буфера выводятся как 480×400 опосредованно.
- Вершинный шейдер / реальные атрибутные буферы **не подключены**; используется
  DMA fallback с нулюадратами (README L125, L129–130).
- `Texture0` не привязана для логотипа/карты (README L131).
- В захваченных PICA‑списках (32 … 29 824 Б) **нет ни одного draw trigger**
  (`0x22E`/`0x22F`), framebuffer чёрный (README L34).

### 5.3 GSP
- Зависание overlay/stalled‑GSP при ожидании VSYNC/VBlank и смены буферов —
  активно расследуется (`docs/gsp-overlay-stall.md`); требует доработки планировки
  кадров и потребления GX‑очереди.

### 5.4 Сборка / воспроизводимость
- `src/native_fs.c`, `src/native_hid.c` и три тестовых скрипта
  (`test_native_fs.py`, `test_native_hid.py`, `test_pica_texture.py`) — **untracked**,
  не попали в git и не в `.gitignore`. Любой чистый `git checkout`‑билд упадёт на
  `include_str!("src/native_fs.c")` / `include_str!("src/native_hid.c")`.
- `recomp-roots.txt`, `src/main.rs`, `src/pica_vs.c` — **модифицированы в рабочем
  дереве** (HEAD `ec16f8c`): `recomp-roots.txt` +115 корней; `main.rs` +66 строк
  (wait_arbiter/priority + multi‑object waits, `ec16f8c`); `pica_vs.c` +11 строк.
- Воспроизводимость доказана: `cargo build --release` + `cargo test` + все 6 наборов
  + полный `recompile-arm`‑конвейер работают из текущего рабочего дерева (см. §3, §6).

### 5.5 Качество кода
- `dq8-tool` имеет 1 unit‑тест (bflim L8 unswizzle) в `src/bin/dq8-title.rs`;
  `src/main.rs` — 34 unit‑теста (см. шапку файла). Их покрытие SVC/планировщика/PICA
  ограничено, но **все 35 тестов now verified green** (`cargo test`).
- `tools/escape_loop.sh` ловит `unimplemented` в stderr — есть «побег» из
  зависимостей, но не автопокрытие.

---

## 6. Оценка тестового покрытия

| Набор | Что проверяет | Статус (устар. бинарь 19:08) | Статус (пересборка 04:14) |
|---|---|---|---|
| `test_pica_vs` | VS: матрицы, dot, normalize | PASS (2/2) | **PASS (2/2)** ✅ |
| `test_pica_texture` | BFLIM fmt 4/6/7/8/9/10/11, morton/tiling | SIGABRT | **PASS (1/1)** ✅ |
| `test_apt_ipc` | APT IPC (Initialize/Enable/SharedFont) | PASS | **PASS (1/1)** ✅ |
| `test_waitn` | WaitSync(N), arbiter Signal, file-handle isolation, resume | compile‑fail (stale) | **PASS (1/1)** ✅ |
| `test_native_fs` | SaveData create/write/reopen/EOF/dir/escape | SIGABRT (stale) | **PASS (1/1)** ✅ |
| `test_native_hid` | HID ring, press/release, events | compile‑fail (stale) | **PASS (1/1)** ✅ |

**Итого: 6/6 наборов green + 35/35 `cargo test` после пересборки из исходников.**
Ранее «4/6 падений» были слеплены на устаревшем бинаре 19:08 (не включал
`native_fs.c`/`native_hid.c` и структуру `GuestThread` без `wait_arbiter`/`priority`);
пересборка из `ec16f8c` + рабочего дерева устраняет все падения без правок исходника.

Нет покрытия: GSP overlay/VSYNC, BFLAN анимация заголовка, сквозной путь до
видимого титульного кадра, end‑to‑end на 2 234 восстановленных функциях.

---

## 7. Приоритизированный план развития

### P0 — блокеры (без этого нельзя верифицировать и нельзя идти дальше)
1. **[ГОТОВО ✅, P0.1 закрыта]** Тулчейн установлен и проверен: `cargo 1.99.1`,
   `rustc 1.99.1`, `clang 18.1.3`, `gcc 13.3.0`, `ctrtool`, `sdl2-config 2.30.0`
   — все на хосте (см. §3). README L112 теперь достоверен.
2. **Закоммитить незакоммиченные артефакты**, чтобы сборка и тесты были
   воспроизводимы: `src/native_fs.c`, `src/native_hid.c`,
   `tools/test_native_fs.py`, `tools/test_native_hid.py`,
   `tools/test_pica_texture.py`, пополненный `recomp-roots.txt` (+115 корней).
3. **[ГОТОВО ✅, P0.3 выполнена]** `cargo build --release` → exit 0 (8.39 с);
   `cargo test` → **35/35 ok**; все 6 наборов `tools/test_*.py` → **6/6 PASS**
   (exit 0). Никаких регрессий в текущем исходнике выявлено.

### P1 — ближайшая играбельная веха (видимый титульный кадр)
4. **Подключить реальный PICA VS + атрибутные буферы** вместо DMA unit‑quad fallback
   (README L129–130).
5. **Разобрать `pica_regs[0x11E]` (цветовые width/height) и `@0x11D` dim** — main.rs:3728.
6. **Texture0 bind + BFLIM logo/map** (README L34, L131): убрать «framebuffer black»,
   включить draw triggers `0x22E`/`0x22F` в захватах.
7. **GSP overlay stall / VSYNC‑VBlank пейзинг** (`docs/gsp-overlay-stall.md`).
8. **Довести путь `title002_u.bflyt` → `Visible_` → BFLAN title001/000** до появления
   титульного экрана и проверить request savedata/intro movie (README L34).

### P2 — корректность и покрытие (меньше риска, широкий эффект)
9. **Реализовать недостающие SVC** (таблица §5.1). Особенно `SignalEvent`/`ClearEvent` —
   сейчас `exit(23/24)`, что ранит любые игровые пути. Далее `Map/UnmapMemoryBlock`,
   `GetProcessId`, `CloseHandle`, `GetServiceHandle`, `ConnectToPort`/`SendSyncRequest`.
10. **Улучшить recovery функций**: jump tables, register‑based indirect calls,
    code/data границы (README L32); расширить 379‑entry indirect‑target таблицу.
11. **Юнит‑ и интеграционные тесты на GSP capture + framebuffer** + проверка, что
    захваченные PICA‑списки содержат draw triggers.

### P3 — устойчивость и CI
12. **[выполнено ✅]** `clang` — теперь реальный компилятор 18.1.3 (`/usr/bin/clang`),
    а не gcc‑шим. Полный `recompile-arm`‑вывод собирается им с SDL2 и в headless.
13. **[выполнено ✅]** CI‑затычка headless (`DQ8_HEADLESS=1`) проверена: нативные тесты
    и сборка работают без дисплея (см. §3).
14. Дать CI‑запуск вспомогательным скриптам (`escape_loop.sh`, `make_gdb_trace.py`,
    `romfs_read_report.py`, `pica_list_report.py`, `bflim_to_png.py`,
    `decode_sqex_logo.py`) и документировать `Azahar GDB stub`‑поток (README L58).

---

## 8. Матрица ответственности (кто делает)

- **Ops / администратор ОС:** **[ГОТОВО ✅]** P0.1 — `cargo`/`rustc`/`clang`/`ctrtool`/
  `libsdl2-dev` установлены и проверены на хосте (см. §3). Больше нет блокера сборки.
- **Recompiler (coder):** P0.2 (commit незакоммиченных модулей — `native_fs.c`/
  `native_hid.c`/тесты/`recomp-roots.txt`) → P1 (видимый титульный кадр: PICA VS,
  Texture0, bflyt, draw triggers) → P2 (недостающие SVC, recovery функций).

---

## 9. Риски и допущения

- Все цифры (181 600 cases / 17 003 функций / 16.1 % / 2 234 candidate‑функций)
  взяты из `README.md` L32–34 — это результаты, описанные автором; в текущей сессии
  **cargo есть**, и полную пересборку `recompile-arm` удалось воспроизвести:
  259 231 инструкций / 24 206 функций (см. §3). Числа из README не пересчитаны вручную,
  но конвейер воспроизводим end‑to‑end.
- `DQ8.code` присутствует на диске (5 218 304 Б) и `disasm` на нём работает;
  `decompile` и `decrypt-code` не запускались в этой сессии (требуют ключи из
  `aes_keys.txt`, лежащие в `.gitignore`, и не читались).
- Диагноз «4/6 тестов падают из‑за устаревшего бинаря» **подтверждён и устранён**:
  после `cargo build --release` все 6 наборов и 35 unit‑тестов зелёные (см. §6).
  Исходный код **не изменялся** в рамках проверки.
- **Реальный блокер остался** — игра не выводит видимый титульный кадр (§4):
  нулюадраты вместо треугольников PICA, Texture0 не привязана, нет draw triggers,
  `title002_u.bflyt` застревает на `Visible_` lookup. Это требует правок в `main.rs`
  (PICA/Texturing) — вне рамок этой проверочной задачи (кода не меняем).
- Воспроизводимость: `native_fs.c`, `native_hid.c`, 3 тестовых скрипта и
  пополненный `recomp-roots.txt` (+115) **всё ещё не закоммичены** (см. §5.4 / P0.2).
  Их нужно закоммитить, иначе сборка из чистого git‑checkout упадёт на `include_str!`.

---

## 10. Что уже сделано в рамках этой сессии (2026-10-20)

Проверка «сворачивания после установки тулчейна». Исходный код **не изменялся**.

- **Тулчейн**: `cargo 1.99.1`, `rustc 1.99.1`, `clang 18.1.3`, `gcc 13.3.0`,
  `ctrtool`, `sdl2-config 2.30.0` — все на хосте ✓.
- `cargo build --release` → **exit 0**, 8.39 с, бинарь `target/release/dq8tool`
  (1 057 120 Б, 04:14 — новее HEAD `ec16f8c`).
- `cargo test` → **35/35 ok** (34 `main.rs` + 1 `dq8-title.rs`).
- **6/6 нативных наборов** `tools/test_*.py` против пересобранного бинаря → **все PASS
  (exit 0)**; ранее «красные» 4 набора теперь зелёные без правок исходника.
- `disasm DQ8.code --mode arm --base 0x00100000 --count 16` → корректный псевдокод.
- `recompile-arm stub.code 0x00100000 stub.c` → C с шаблонами, компилируется `clang`-ом
  и `gcc`-ом (exit 0 оба).
- **Полный конвейер** `recompile-arm DQ8.code … (1236 roots)`: 259 231 инструкция /
  24 206 функций → 264 596 строк C → компилируется и линкуется `clang + SDL2` и в
  headless‑режиме (exit 0 оба).
- `GuestThread.wait_arbiter`/`priority` (main.rs:3596–3597) подтверждены в исходнике;
  `native_fs.c`/`native_hid.c` подтверждены на диске и задействованы через `include_str!`.
- **Что НЕ сделано (вне зоны этой задачи):** игра всё ещё не достигает видимого
  титульного кадра (§4); незакоммиченные артефакты не закоммичены (P0.2 — нужен
  отдельный проход committer'ом).

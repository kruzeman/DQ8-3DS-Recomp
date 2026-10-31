# DQ8: ложный переход в HOME и зависание APT (19.10.2026)

Ветка: `codex/fix-apt-home-resume`. Снимок входящих изменений другого агента:
`eac6fa6`. ROM, ключи, ресурсы и исходники эмулятора в коммиты не добавлялись.

## Подтверждённая причина HOME

`APT:GetLockHandle` записывал только Result и mutex. Поля corrected_attributes
и state оставались от прежней SRV-команды. В state попадало `0x66630041`
(остаток имени сервиса). ARM-обёртка `0x0010D714` читает state из IPC +0x8C,
а `0x00106F60..0x00106F64` передаёт его младший бит в `0x00115860`.
Та записывает `0x005D0315 = 1`. Проверка `0x0010D49C`, вызванная из
`0x0010809C`, разрешает путь `0x0010D2E4 -> 0x00115880 -> JumpToHomeMenu`.

До исправления `/tmp/dq8-home-cause.err` показывает запись HOME-флага в 1 и
повторные входы в `0x001080AC`. После исправления
`/tmp/dq8-apt-v3-run/run.err` показывает запись того же флага в 0 и **ноль**
вызовов `JumpToHomeMenu` за 40 секунд. Для объяснения этого конкретного
перехода отсутствие профиля пользователя не требуется. Полноценный путь
создания сохранения этим ещё не проверен.

## Исправления

- GetLockHandle: header `0x000100C2`, Result, attributes, state=0,
  copy-descriptor, настоящий mutex из таблицы runtime.
- Initialize: header `0x00020043`, два настоящих события APT вместо инертных
  handles; parameter event сигнализируется при наличии сообщения.
- ReceiveParameter/GlanceParameter: header `0x000D0104`/`0x000E0104`, включая
  static-buffer descriptor и адрес. Receive потребляет сообщение один раз,
  пустой mailbox возвращает APT NoData (`0xC8A0CFEF`).
- Headless HOME round trip возвращает `WakeupByPause` (0xB), как
  `AppletManager::LeaveHomeMenu` в локальном Azahar. Старый Response=3 не
  возобновляет приложение, поэтому прежний ReceiveParameter повторялся.
- Удалены записи в guest startup-object, сигнализация чужого GSP-события и
  принудительная кнопка A из HOME-заглушки.
- AppletUtility: нулевой ответ, кроме true для TryLockTransition=6;
  исправлены IPC header и descriptor. Это отдельная ошибка протокола;
  её исправление само по себе цикл HOME не убрало.

Сверено с локальными `azahar-master/src/core/hle/service/apt/apt.cpp`
(GetLockHandle, Initialize, ReceiveParameter, AppletUtility) и
`applet_manager.cpp` (LeaveHomeMenu).

## Проверка и текущая граница

34 Rust-теста, два native PICA-теста ASan/UBSan и новый native APT-тест прошли.
`tools/test_apt_ipc.py` генерирует C из ARM bx lr и вызывает именно сервисы
сгенерированного runtime. Проверяет загрязнённый GetLockHandle, события,
однократное потребление, Glance, возврат из HOME и отсутствие записей в
память игры/HID. Запуск:

```
DQ8_TOOL=/tmp/dq8-apt-build/release/dq8tool python3 tools/test_apt_ipc.py
```

Реальный runner: `/tmp/dq8-apt-v3`, C: `/tmp/dq8-apt-v3.c`.
Лог: `/tmp/dq8-apt-v3-run/run.err`.
За ограниченный 40-секундный запуск: **614 draws, 420 display transfers,
0 HOME calls**. Rasterizer включён, оверлеи и искусственная геометрия не
использовались. Затем снова прежнее ожидание: worker 11 ждёт адрес
`0x087F5968`, main в пути pending-job. Титульный экран и внутриигровой запрос
создания сохранения пока не подтверждены.

## Не смешивать с исправлением HOME

В текущем входящем FS-коде ещё есть отдельные проблемы для следующей работы:
OpenArchive должен возвращать u64 archive handle (3 normal words), а не
move-handle descriptor. Фиктивная Directory:Read всегда выдаёт honeySave.bin
без EOF; чтение такого файла выдаёт нули, не сохранённые ранее записи.
Несколько независимых file/event counters также используют диапазон 0x20.
Эти участки данным патчем не менялись. Два A нужно подавать через корректный
HID после появления настоящего внутриигрового диалога.

# CS2 ESP Overlay — журнал разработки

Статус: в процессе, учебный проект (C/C++ + AI), обновляется по мере тестов.
Смотри также `DOCS/Investigation.md` — более ранний отчёт по офсетам/структуре сущностей (актуален как история, но раздел "Текущее состояние" ниже — самый свежий).

## 🎯 Цель проекта

Внешний (external, без инжекта в игру) ESP-оверлей для CS2:
- прямоугольники вокруг игроков (Box ESP)
- полоски здоровья
- разделение враг/союзник по команде

## 📁 Структура проекта

```
CS2-ESP-Overlay/
├── cs_main.cpp          # Основная программа: оверлей + чтение памяти + ESP
├── pattern_scanning.cpp # Отдельный сканер памяти (паттерны + динамический value-scan)
├── RadarHackEx1.cpp     # Ранний учебный прототип (поиск client.dll, вывод игроков в консоль)
├── secret/              # Обмен данными между cs_main.exe и pattern_scanning.exe (ccs.trs / poc.trs)
├── output/               # Генерация cs2-dumper.exe (offsets.hpp/json, client_dll.hpp/json, ...)
└── DOCS/
    ├── Readme.md          # этот файл — журнал/текущий статус
    ├── Investigation.md   # более ранний детальный отчёт по этапам 1-4
    └── ReadMe1.md         # учебные заметки: WorldToScreen, способы отрисовки (GDI/DirectX/Overlay)
```

## 🧩 Как устроено чтение памяти (архитектура)

1. `cs2-dumper.exe` генерирует `output/offsets.json` и `output/client_dll.json` — это **schema-офсеты** (поля классов, которые движок сам публикует через reflection: `m_iHealth`, `m_iTeamNum`, `m_vecOrigin`, `m_pGameSceneNode` и т.д.) и несколько глобальных указателей (`dwEntityList`, `dwLocalPlayerPawn`, `dwViewMatrix`, `dwGameEntitySystem`).
2. `cs_main.cpp` при старте запускает `cs2-dumper.exe`, парсит JSON (`LoadOffsetsFromDumper`) и получает актуальные офсеты без ручного вбивания констант.
3. Локальный игрок читается напрямую: `clientBase + dwLocalPlayerPawn` → указатель на пешку → `+0x330` (`m_pGameSceneNode`) → `+0x80` (`m_vecOrigin`).
4. Остальные сущности (боты/враги) читаются через `dwGameEntitySystem` — самый проблемный участок, см. ниже.
5. Отрисовка — GDI, прозрачное окно поверх игры (`WS_EX_LAYERED | WS_EX_TRANSPARENT`), `WorldToScreen` через `dwViewMatrix` (4×4 матрица).

## 📜 Краткая история (этапы 1-4, подробности — в Investigation.md)

| Этап | Что делали | Итог |
|---|---|---|
| 1 | Базовый GDI-оверлей + `WorldToScreen` | ✅ Работает, но не видит игроков |
| 2 | `cs2-dumper` для офсетов | ✅ Офсеты подгружаются, но не все нужные поля есть в дампе (`dwGameEntitySystem` не dump-ится напрямую) |
| 3 | Переход на `dwGameEntitySystem` | Нашли `entitySystem+0x10` как начало списка, но с неверной арифметикой (см. ниже) |
| 4 | Первые попытки читать игроков | ❌ Health/Team — мусор, т.к. сущности выбирались неправильно |

## 🔍 Сессия 2026-09-06 — найдена и исправлена главная ошибка

### Диагноз

Все schema-офсеты (`m_iHealth=0x34C`, `m_iTeamNum=0x3E7`, `m_lifeState=0x354`, `m_pGameSceneNode=0x330`, `m_vecOrigin=0x80`) — **правильные**, сверены с дампом `output/client_dll.hpp`. Проблема была не в них.

Настоящий баг — в обходе `CGameEntitySystem` (`cs_main.cpp`, функция `GetPlayers`). Старый код:

```cpp
// БЫЛО (неверно):
uintptr_t listEntry = 0;
ReadMemory(hProcess, entitySystem + 0x10 + i * 0x8, listEntry);
uintptr_t entity = 0;
ReadMemory(hProcess, listEntry + 0x0, entity);
```

`entitySystem + 0x10` (`m_EntityPtrArray`) — это не плоский массив сущностей, а массив указателей на **чанки по 512 сущностей**. Внутри чанка сущности лежат структурами `CEntityIdentity` размером `0x78` (120) байт, а указатель на саму сущность (`m_pInstance`) — первое поле такой структуры (смещение `0x0`).

Старый код для каждого `i` читал не следующую сущность внутри чанка, а **следующий указатель на чанк** (шаг `0x8` вместо `0x78`), из-за чего дальше первого-двух индексов начинался мусор — отсюда "Health passed: 0, Team passed: 0" из Investigation.md.

### Исправление

Добавлена функция `GetEntityByIndex` с правильной двухуровневой арифметикой:

```cpp
constexpr int ENTITIES_PER_CHUNK = 512;      // 0x200
constexpr int ENTITY_IDENTITY_SIZE = 0x78;   // 120 байт

uintptr_t GetEntityByIndex(HANDLE hProcess, uintptr_t entitySystem, int index) {
    int chunkIndex = index / ENTITIES_PER_CHUNK;   // index >> 9
    int entryIndex = index % ENTITIES_PER_CHUNK;   // index & 0x1FF

    uintptr_t chunkPtr = 0;
    if (!ReadMemory(hProcess, entitySystem + 0x10 + chunkIndex * 0x8, chunkPtr) || !IsValidAddress(chunkPtr))
        return 0;

    uintptr_t entity = 0;
    ReadMemory(hProcess, chunkPtr + entryIndex * ENTITY_IDENTITY_SIZE, entity);
    return entity;
}
```

`GetPlayers` теперь вызывает `GetEntityByIndex(hProcess, entitySystem, i)` вместо ручного расчёта.

**Формула подтверждена независимым источником** — публичный репозиторий `aci1337/CS2-External` (`Entity.cpp`) использует буквально ту же арифметику:
```cpp
ReadMemory<DWORD64>(EntityPawnListEntry + 0x10 + 8 * ((Pawn & 0x7FFF) >> 9), EntityPawnListEntry);
ReadMemory<DWORD64>(EntityPawnListEntry + 0x78 * (Pawn & 0x1FF), EntityPawnAddress);
```

### Дебаг-инструментарий, добавленный в `GetPlayers`

Раз в ~60 кадров (`isDebugFrame`) выводится:
```
[DEBUG] entitySystem=0x... highestIndex=... rawValidEntities=... passedPlayerCheck=... totalPlayers=...
```
и для каждой найденной "сырой" сущности (сейчас лимит поднят до 60, чтобы видеть все):
```
[DEBUG]   idx=N entity=0x... vtable=0x... (vtable-clientBase=0x...) health=... team=... lifeState=...
```
`vtable` — первые 8 байт по адресу сущности; если это указатель внутрь `client.dll` (маленькая разница `vtable-clientBase`), сущность настоящая. Если `vtable` — большое число/невыровненный адрес, это мусор (пустой/освобождённый слот), и такие сущности должны отсеиваться `IsPlayerEntity`.

### Первый тест после фикса

`entitySystem` и `highestIndex=175` читаются корректно. `rawValidEntities=41` из 175 индексов — уже осмысленное число (правдоподобно для матча с ботами: игроки + оружие + пропы). `idx=0` — это `world`-сущность, читается идеально (`vtable` внутри `client.dll`, health=0/team=0 — так и должно быть у world). Но `passedPlayerCheck=0` — ни один из 41 не прошёл проверку на игрока.

**Причина не в фильтре и не в игре** (пользователь подтвердил: матч с ботами шёл, боты двигались, локальный игрок и раньше читался верно — HP/Team/позиция). Причина — в дебаг-выводе: он был искусственно ограничен первыми **6** сущностями по порядку индекса (0,1,4,10,11,24 — это не игроки, а статичные world/пропы), хотя всего валидных было 41. Реальные боты почти наверняка среди оставшихся ~35, у которых индексы больше 24, и их просто не было видно в логе.

## ❗ Текущий блокер / следующий шаг

Лимит вывода в дебаге поднят с 6 до 60 (`if (isDebugFrame && dbgPrinted < 60)`), теперь должны быть видны все ~41 сущности за кадр. **Нужно пересобрать и заново прогнать тест в матче с ботами**, прислать полный лог — по нему должно стать видно:
- есть ли среди оставшихся сущностей адреса с `vtable` внутри `client.dll` и health/team в реалистичном диапазоне (это и будут боты);
- если да — почему `IsPlayerEntity` их всё равно отсеивает (может, `m_lifeState` для ботов не равен ровно 0, или контроллеры/пешки нужно различать отдельно);
- если и там пусто — придётся перепроверять `dwGameEntitySystem_highestEntityIndex` (`0x2090`) и, возможно, искать сущности по `dwLocalPlayerController`-подобному пути через контроллеров (`m_hPlayerPawn`), а не по общему списку.

## 🛠️ Команда сборки

```
g++ -g cs_main.cpp -o cs_main.exe -lgdi32 -luser32 -lpsapi -static
g++ -g pattern_scanning.cpp -o pattern_scanning.exe -lgdi32 -luser32 -lpsapi -static
```

## 📈 Прогресс

```
[████████████████████████████████████████░░░░] ~85%

✅ Оверлей работает
✅ WorldToScreen работает
✅ Офсеты загружаются из cs2-dumper (все сверены и верны)
✅ Позиция локального игрока читается
✅ Chunk-арифметика CGameEntitySystem исправлена и подтверждена внешним источником
✅ Дебаг-инструментарий для диагностики сущностей добавлен
❓ Не подтверждено: находятся ли живые боты среди 41 сырых сущностей (ждём новый лог)
❌ ESP игроков на экране пока не подтверждён вживую
```

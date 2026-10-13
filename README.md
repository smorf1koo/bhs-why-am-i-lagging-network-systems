# Network / Systems Engineer: «Измеряем user-to-user latency»

Тестовое задание проекта **Why_Am_I_Lagging** (Bear Head Studio).

## О задании

Готовый авторитетный UDP-сервер с тиком 60 Гц и встроенным эмулятором задержки, джиттера и потерь
уже написан. Вы пишете клиенты на C++ или Python, измеряете user-to-user latency для каждой пары
игроков на трёх сетевых профилях и объясняете цифры в коротком отчёте.
Root, Docker и VM не нужны: macOS или Linux; Windows — через WSL2
(сервер использует POSIX-сокеты и под MSVC / MinGW не собирается).

Задание — в документе
[`Тестовое_задание_Network_Systems_Engineer.docx`](Тестовое_задание_Network_Systems_Engineer.docx).

## Состав пакета

| Файл / папка | Назначение |
|---|---|
| `Тестовое_задание_Network_Systems_Engineer.docx` | задание: задача, входные данные, что сделать, как сдать |
| `starter/` | CMake-проект: **готовый** `wail_server` (UDP-сервер с эмулятором сети), протокол и часы в `common/`, скелет `client/main.cpp`, тест `protocol_selftest` |
| `starter/python/wail_protocol.py` | готовый протокол на Python и `mono_ns()`, совпадающий с часами сервера |
| `protocol.md` | бинарный формат сообщений (протокол v1) и обязательная семантика |
| `profiles.yaml` | 3 сетевых профиля (`lan`, `wan_jittery`, `asymmetric_bad_client`) — справочно, значения зашиты в сервер |
| `tools/plot_percentiles.py`, `tools/log_format.md` | p50 / p95 / p99 и доля «не увидено» по итоговому CSV; формат CSV и логов сервера |
| `tools/example/u2u_example.csv` | пример итогового CSV: короткий прогон профиля `lan` |
| `report_template.md` | шаблон `report.md` |

## Как начать

```bash
cd starter
cmake -B build && cmake --build build -j   # CMake >= 3.20, компилятор с C++20
ctest --test-dir build                     # protocol_selftest
./build/wail_server --list-profiles
```

Без CMake сервер собирается одной командой (из `starter/`, вместо `clang++` подойдёт `g++`):

```bash
mkdir -p build && clang++ -std=c++20 -O2 -Icommon/include server/main.cpp common/src/protocol.cpp -o build/wail_server
```

Для клиента на Python (из `starter/`): `python3 -I python/wail_protocol.py` — самопроверка, выводит `OK`.

## Запуск

```bash
./build/wail_server --profile asymmetric_bad_client --duration 120 --exit-when-empty --log-dir runs/asym
```

- `--profile P` — сетевой профиль: `lan`, `wan_jittery`, `asymmetric_bad_client` или `none` (без эмуляции);
- `--duration N` — сервер завершается через N секунд (берите с запасом на подключение, прогрев и «дренаж»);
- `--exit-when-empty` — сервер завершается раньше, как только все клиенты прислали `Bye`;
- `--log-dir DIR` — куда записать `server_ticks.csv` и `server_inputs.csv` (формат — `tools/log_format.md`).

Сервер слушает UDP `127.0.0.1:40000`. При выходе он печатает сводку: число клиентов, принятые вводы,
фактические задержки и потери по клиентам и направлениям. Остальные ключи — `wail_server --help`.

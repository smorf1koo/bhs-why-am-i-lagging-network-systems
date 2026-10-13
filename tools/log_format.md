# Формат итогового CSV

`tools/plot_percentiles.py` читает **итоговый** CSV (один или несколько файлов) и печатает
p50 / p95 / p99 и долю «не увидено» по профилям и по парам A → B. Как вы получите этот файл —
сразу в клиенте или отдельным скриптом из сырых логов — решаете сами (код должен быть в репозитории).

## Одна строка = одно наблюдение «ввод A увидел клиент B»

UTF-8, разделитель `,`, первая строка — заголовок. Порядок колонок произвольный, лишние колонки игнорируются.

| Колонка | Обяз. | Тип | Смысл |
|---|---|---|---|
| `profile` | да | str | имя сетевого профиля (`lan`, `wan_jittery`, `asymmetric_bad_client`) |
| `src_client` | да | int | client_id отправителя ввода (A) |
| `dst_client` | да | int | client_id наблюдателя (B), `dst_client != src_client` |
| `input_seq` | да | int | `input_seq` ввода A |
| `u2u_ms` | да | float | user-to-user latency, мс. **Пусто**, если B так и не увидел этот ввод |
| `run_id` | нет | str | идентификатор прогона |
| `clock_method` | нет | str | как сделаны сравнимыми часы: `shared_monotonic`, `ping_offset`, ... |
| `clock_err_ms` | нет | float | оценка погрешности часов для строки |
| `uplink_ms`, `tick_wait_ms`, `server_ms`, `downlink_ms` | нет | float | (плюс) компоненты задержки; если заполнены, скрипт проверит, что их сумма ≈ `u2u_ms` |

**Определение события** (protocol.md, «Семантика»): ввод A с номером `k` считается учтённым у B в момент,
когда B впервые получает `State`, где для A `last_input_seq >= k`. Если вы выбрали другое определение — объясните.

Пример формата: `tools/example/u2u_example.csv` — короткий прогон профиля `lan` (10 с, 3 клиента, без колонок компонент).

## Логи сервера (готовые, `wail_server --log-dir DIR`)

* `server_ticks.csv` — `tick,scheduled_ns,tick_start_ns,send_done_ns,players`;
* `server_inputs.csv` — `client_id,input_seq,input_ns,sock_recv_ns,deliver_ns,fate`, где
  `sock_recv_ns` — датаграмма пришла в сокет сервера, `deliver_ns` — вышла из эмулятора uplink
  (= момент, когда её видит логика сервера), `fate` ∈ `accepted | stale | dropped_up | unknown_client`.

Все `*_ns` сервера — `wail::mono_ns()` (= `std::chrono::steady_clock`) процесса сервера.
Они нужны только для плюса (разложение на компоненты); для базовой части не обязательны.

# Bluetooth protocol

The FIRE advertises as `Tock-XXXX` with one GATT service. Every characteristic needs an
encrypted, authenticated link (LE Secure Connections with a passkey), so the first connection
pairs: the FIRE shows a six-digit code and the central types it in.

Service `7a0c0001-4c3f-4d7e-9b6a-70c6f1a0c0de`:

| UUID suffix | Name | | |
| --- | --- | --- | --- |
| `0002` | STATUS | read | JSON: name, battery, charging, clock known, Wi-Fi state, network, IP, the API key's last four characters, model, `goalDay`, `goalMin`, and `timer` (state, break, seconds left and total, round, rounds, task) |
| `0003` | STATS | read | binary, the last 112 days (below) |
| `0004` | COMMAND | write | one text command per write (below) |
| `0005` | TASKS | read | text: `cur:<id>`, then one `id\|color\|name` line per task |
| `0006` | MESSAGES | read | text: the screensaver's lines, one per line |
| `0007` | TASKSTATS | read | binary, per task, the last 14 days (below) |

All suffixes share the prefix `7a0c`, then the suffix, then `-4c3f-4d7e-9b6a-70c6f1a0c0de`.

## Commands

| | |
| --- | --- |
| `time:<epoch>` | set the clock (UTC seconds) |
| `key:<openai key>` | store the API key for Talk (empty removes it) |
| `model:<name>` | the model Talk uses |
| `wifi:<ssid><tab><password>` | save and join a network |
| `tasks:<lines>` | replace the task list: `id\|color\|name` per line, id 0 for a new task |
| `msgs:<lines>` | replace the screensaver's lines (none = the defaults) |
| `goal:<hours>`, `goalmin:<minutes>` | today's goal, 15 minutes to 16 hours |

## Binary formats

Little-endian.

STATS: `u8` version (1), `u8` daily goal in whole hours, `i32` today (days since 1970-01-01 on
the FIRE's calendar, -1 while the clock is unknown), `u16` count, then per day, oldest first:
`u16` focused minutes, `u8` finished sessions.

TASKSTATS: `u8` version (1), `i32` today, `u8` count, then per task: `u8` id, then 14 x `u16`
focused minutes, oldest first. Untagged time is the day's total minus these.

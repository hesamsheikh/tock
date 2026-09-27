# Bluetooth protocol

The FIRE advertises as `Tock-XXXX` with one GATT service. Every characteristic needs an
encrypted, authenticated link (LE Secure Connections with a passkey), so the first connection
pairs: the FIRE shows a six-digit code and the central types it in.

Service `7a0c0001-4c3f-4d7e-9b6a-70c6f1a0c0de`:

| UUID suffix | Name | | |
| --- | --- | --- | --- |
| `0002` | STATUS | read | JSON: firmware version, name, battery, charging, clock known, Wi-Fi state, network, IP, the API key's last four characters, model, `goalDay`, `goalMin`, and `timer` (state, break, seconds left and total, round, rounds, task) |
| `0003` | STATS | read | binary, the last 112 days (below) |
| `0004` | COMMAND | write | one text command per write (below) |
| `0005` | TASKS | read | text: `cur:<id>`, then one `id\|color\|name` line per task |
| `0006` | MESSAGES | read | text: the screensaver's lines, one per line |
| `0007` | TASKSTATS | read | binary, per task, the last 14 days (below) |
| `0008` | UPDATE | write without response, read | firmware updates (below) |

All suffixes share the prefix `7a0c`, then the suffix, then `-4c3f-4d7e-9b6a-70c6f1a0c0de`.

## Commands

| | |
| --- | --- |
| `time:<epoch>` | set the clock (UTC seconds) |
| `tz:<POSIX TZ>` | the time zone, like `STD-1DST-2,M3.5.0,M10.5.0/3`: when days start (kept on the FIRE) |
| `key:<openai key>` | store the API key for Talk (empty removes it) |
| `model:<name>` | the model Talk uses |
| `wifi:<ssid><tab><password>` | save and join a network |
| `tasks:<lines>` | replace the task list: `id\|color\|name` per line, id 0 for a new task |
| `msgs:<lines>` | replace the screensaver's lines (none = the defaults) |
| `goal:<hours>`, `goalmin:<minutes>` | today's goal, 15 minutes to 16 hours |
| `update:<size> <sha256 hex> [<deflated size>]` | start a firmware update (deflated: the image comes compressed) |
| `update:end` | all of it sent: check it and restart into it |
| `update:cancel` | stop an update; the FIRE keeps its firmware |

## Binary formats

Little-endian.

STATS: `u8` version (1), `u8` daily goal in whole hours, `i32` today (days since 1970-01-01 on
the FIRE's calendar, -1 while the clock is unknown), `u16` count, then per day, oldest first:
`u16` focused minutes, `u8` finished sessions.

TASKSTATS: `u8` version (1), `i32` today, `u8` count, then per task: `u8` id, then 14 x `u16`
focused minutes, oldest first. Untagged time is the day's total minus these.

UPDATE, written: `u32` offset, `u32` FNV-1a of the bytes, then the bytes (up to 504), in order.
Offsets count what's sent: the deflated stream when the update is deflated. The FIRE drops a
chunk that isn't at the next offset, or doesn't match its FNV-1a, or doesn't fit in its 16 KB
buffer yet; the sender reads UPDATE now and then and goes back to `got`.

UPDATE, read: `u8` state (0 idle, 1 receiving, 2 checking, 3 installed and restarting, 4 failed,
5 getting ready), `u8` error (1 too big, 2 no memory, 3 wrong size, 4 wrong hash, 5 flash, 6
timed out, 7 cancelled, 8 didn't inflate), `u32` got (bytes received), `u32` size (of what's
sent), `u32` written (image bytes in flash).

## Firmware updates

1. `update:<size> <sha256> <deflated size>` on COMMAND. The FIRE shows the update screen, makes
   room (state 5), and is ready for the image when UPDATE reads state 1. The size and SHA-256
   are the image's; the deflated size is what will come: the image as raw DEFLATE (RFC 1951),
   about 2/3 of it. Leave it out to send the image as it is.
2. What's sent, in chunks on UPDATE, without waiting for each: at most 12 KB past the last `got`
   read back, which the FIRE's buffer always has room for. The FIRE inflates it with the
   decoder in the ESP32's ROM and writes it to its other app partition as it comes.
3. `update:end` once `got` is the size. When all of it is in flash, the FIRE checks the SHA-256,
   switches to the new firmware and restarts (state 3).
4. The new firmware runs on probation: if it crashes or is reset in its first 15 seconds, the
   bootloader goes back to the old one. The sender knows it worked when STATUS shows the new
   `version`.


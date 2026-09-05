---
name: agentic-logging
description: How every project in this repo logs, so that the agent reading the serial console or the SD flight recorder sees everything it needs without a person at the board. Apply when starting a project, adding a module, or when a log is not telling you enough.
---

# Agentic-first logging

The person at the desk is not the primary reader of this board's logs; the agent is,
usually over a non-resetting serial attach, often after the fact from the SD card. Every
project logs for that reader.

## Configuration (in `sdkconfig.defaults`)

```
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y       # DEBUG compiled in for every component
CONFIG_LOG_DEFAULT_LEVEL_INFO=y        # third-party stays quiet by default
CONFIG_LOG_COLORS=n                    # no escape codes in files or greps
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096   # Wi-Fi/netif DEBUG lines overflowed 2.3 KB
```

`01_project_template` carries these. Then in `app_main`, first thing:

```c
static const char *const own_tags[] = {"wordbook", "recog", "book", /* every tag this project defines */};
for (size_t i = 0; i < sizeof(own_tags) / sizeof(own_tags[0]); i++) {
    esp_log_level_set(own_tags[i], ESP_LOG_DEBUG);
}
```

Own tags at DEBUG from the first line; everything else at INFO. A serial command `d`
flips every tag to DEBUG at runtime and back (see *Serial commands*).

## Rules for the lines themselves

- **Never mute a warning to keep the log tidy.** The benign `i2c.master` pull-up notice
  stays; it is documented in CLAUDE.md instead. A muted line is a line the agent cannot
  see when it stops being benign.
- **One tag per module, matching the file name.** `recog`, `cards`, `sdcard`. Greppable.
- **State changes are INFO, with the number that changed.** `mounted at /sdcard in 56 ms:
  SL16G, 15193 MB`, not `SD OK`. Include what you would want if this were the only line.
- **Every decision the code makes is a line.** Rejected as well as accepted:
  `rejected: detected on silence (prob 0.135, floor 0.20)`. Fallbacks say what they fell
  back from and to.
- **A heartbeat every 10 s** with the state an agent wants at a glance:
  `alive: 2026-09-05 17:59:24 heard=4 internal=85779 psram=4481488 card=1 files=1 log=1 words=11`.
  Real time first (see *Clock*). Flat numbers across heartbeats prove no leak; drifting
  ones show one.
- **Periodic measurements, not just events.** The recogniser prints `mic: avg -53.0 dBFS,
  peak -49.2, vad speech 19% of last 5 s` every five seconds. That line answered
  questions no event line could.
- **Self-tests at boot, with PASS/FAIL in the line**, so a boot log alone proves the
  hardware path: `photo self-test: embedded 960x697 JPEG decoded and shown  [PASS]`.
- **Errors say what to do**: `SAFE MODE: 3 crashes in a row. Not touching SD or the
  recogniser. Flash a fix.`

## The flight recorder

Every `ESP_LOG` line is mirrored to `/sdcard/<project>.log` in append mode when a card
is present (`sdlog.c` in `02_word_book_en`): a PSRAM ring captures from the first line
of boot, a low-priority task drains it, `fsync` every 2 s, rotation at
`CONFIG_WORDBOOK_LOG_MAX_KB`, a `===== boot: ... reset reason N, time ... =====` header
per session. The hook must not put a buffer on the calling task's stack (a static one
under esp_log's lock is right); it runs on every task that logs, including the 2 KB
system event task.

`sdlog` has a second channel for **machine-readable records** (JSON Lines): `02_word_book_en`
writes every classifier result with all candidates, a 5-second environment record and a
session record to `<project>.classifier.jsonl`. Text log for reading, JSONL for analysis;
both timestamped with wall time and uptime so they join.

Copy `sdlog.[ch]` (and `clog.[ch]` as the pattern) into a new project rather than
reinventing them.

## Clock

Timestamps are worthless at 1980. `timesync.c` + `pcf85063.c` set real time from NTP
(when Wi-Fi is configured) → the on-board RTC → the build stamp, and write NTP back to
the RTC. Log headers and heartbeats carry `%Y-%m-%d %H:%M:%S`.

## Serial commands

`devcmd.c` reads single characters from the USB console so the agent can drive the
board with `printf 'm' > /dev/cu.usbmodem3101` (port opened with `-hupcl`, see the
`serial-capture` skill — this does not reset the board):

| key | does |
|---|---|
| `i` | one status line, everything at once |
| `d` | toggle DEBUG on every tag (own tags are already DEBUG) |
| `r` | reload content from the card |
| `s` | sleep / wake |
| `m` | maintenance mode on / off (Wi-Fi + HTTP API) |

Keep the letters when adding commands to a new project; add new ones to this table.

## Safe mode

Three consecutive panic/watchdog resets (counted in RTC memory) and the app stops
before peripherals, says so on screen and serial, and keeps the console alive. A
crash loop on this board has taken the USB link down and needed hands at the desk;
the guard is what makes a bad flash recoverable from the chair. Copy it.

## Reading a log

```zsh
grep -E "self-test|PASS|FAIL" x.log        # did the hardware paths prove out
grep -E "alive" x.log                       # leak? state? time?
grep -E "^E \(|^W \(" x.log | grep -v "3Ah\|i2c.master"   # anything unexpected
grep -E "heard|rejected" x.log              # what the recogniser decided and why
grep -E "mic:" x.log                        # ambient level over time
grep -E "===== boot" x.log                  # sessions and their reset reasons
```

A boot with no `W`/`E` lines except the two documented ones, `PASS` on every self-test,
and flat heartbeats is healthy. Anything else is a finding; write it down in the
project README's verified-output section, with the line that showed it.

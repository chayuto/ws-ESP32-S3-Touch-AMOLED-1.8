# 06_sensorous

Measure everything this board can measure and write it to the microSD card:
the IMU, the ambient sound level, both die temperatures, every power rail, and
the whole radio environment — every Wi-Fi BSSID and every BLE address in
earshot, with RSSI.

Design note: [`docs/design/06_sensorous.md`](../../docs/design/06_sensorous.md).

**Status: runs on hardware.** First flashed 2026-09-18 on the V2 board with a
16 GB card in the slot; six faults found and fixed that day (see the git log for
what each one was). The IMU, the microphone, both radios, the card and the Wi-Fi
export are all confirmed. What has *not* been exercised: rotation at the file
cap, retention deleting a generation, a card pulled without an eject, battery
operation, and `locate.py` against Google with a real key.

## Build and flash

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/06_sensorous -B /tmp/ws-amoled-build/06_sensorous \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
idf.py -C projects/06_sensorous -B /tmp/ws-amoled-build/06_sensorous \
  -p /dev/cu.usbmodem101 flash
```

Wi-Fi credentials go in `sdkconfig.defaults.local` (gitignored). They are used
for NTP at boot and for maintenance mode — never for scanning, which never
associates with anything.

## What lands on the card

`/sdcard/sensorous/`, JSON Lines, one object per line:

| File | `t` | Written |
|---|---|---|
| `sensorous.log` | — | every `ESP_LOG` line, mirrored |
| `sensors.jsonl` | `sensor` | every 10 s stationary, 1 s mobile — IMU window, sound level, temps, power, heap, card |
| `radio.jsonl` | `scan`, `ap`, `ble`, `scan_end` | per scan cycle: one line per AP and per BLE device |
| | `census_wifi`, `census_ble` | on demand (`c`, or the dashboard) |
| `power.jsonl` | `power` | every 60 s and on every VBUS change |
| `events.jsonl` | `boot`, `event` | boot, mode change, card in/out, eject, thermal |

Files rotate at their cap (log 4 MB, data 8 MB), keeping 8 generations. Below
64 MB free the oldest generation is deleted, and says so in the log.

## Controls

| | |
|---|---|
| **BOOT short** | switch stationary ↔ mobile |
| **BOOT long** | enter/leave maintenance mode |
| **EJECT on screen** | flush, close, unmount — then the button reads MOUNT |

Serial console: `m` maintenance, `M` mode, `S` scan now, `e` eject, `o` mount,
`c` census dump, `z` census clear, `i` status, `d` all tags to DEBUG, `p` power
rails, `b` full brightness, `x` panel re-init, `t` thermal simulation step.

## Getting the data off

Pull the card, or hold BOOT to enter maintenance mode and open the address the
screen shows:

```
GET  /                one page: metrics, files, census, log tail, buttons
GET  /api/file?name=radio.jsonl          the whole file
GET  /api/file?name=radio.jsonl&from=N   resume at byte N
GET  /api/census?kind=wifi|ble           the address tables, live
POST /api/eject | /api/mount | /api/dump | /api/reboot
```

Scanning stops while maintenance mode is up — one antenna.

## Sound is a level, not a recording

Each sensor record carries `sound: {rms_dbfs, peak_dbfs, clipped, n, ms}` and
nothing else. The microphone is opened for a 200 ms burst and **closed again**
— between bursts the ADC is not running and the I2S channel is not clocked, so
nothing is being captured at all. Samples are read in 512-sample chunks into one
static buffer, folded into running sums and overwritten; no audio is stored,
transmitted, or examined for content, and there is no VAD or recogniser in this
image. See the header comment in `main/sound.h`.

`dBFS` is an uncalibrated electrical level, not `dB SPL`: 0 dBFS is a full-scale
square wave, and there is 30 dB of mic gain in front of it. Readings compare with
each other, not with a sound level meter. A non-zero `clipped` means the reading
is a floor rather than a value — turn `CONFIG_SENSOROUS_MIC_GAIN_DB` down.

Set `CONFIG_SENSOROUS_SOUND=n` to leave the microphone alone entirely.

## Turning the scans into positions

`tools/locate.py` groups the `ap` lines by scan and posts them to Google's
Geolocation API. The records already use Google's field names, so there is no
mapping step.

```zsh
./tools/locate.py radio.jsonl                     # summarise; nothing leaves the machine
./tools/locate.py radio.jsonl --key $GOOGLE_KEY   # resolve every scan
./tools/locate.py radio.jsonl --key $GOOGLE_KEY --every 10 --geojson run.geojson
```

Wi-Fi BSSIDs are the location signal: they are globally unique and they do not
move. **BLE addresses mostly are not** — phones and watches rotate a Resolvable
Private Address every ~15 minutes. Every BLE row records `addrType` so analysis
can keep only `public` and `random_static`; the rest are an occupancy signal, not
a position.

Nothing is uploaded from the board itself, ever.

## Getting a run out and checking it

Two commands, and neither of them opens the card slot:

```zsh
tools/extract.sh          # maintenance mode, download everything, back to work
tools/vv.py               # check what came off: PASS/FAIL per claim, exit 1 on any fail
```

`extract.sh` is incremental — run it again and it fetches only the bytes written
since last time and appends them, so a long run can be sampled as it goes. It
puts the files in `data/` (gitignored) along with the live census tables and the
metrics endpoint. `vv.py` reads the most recent boot by default; `--all` reads
every boot in the files.

## Units, so nothing is read as something it is not

- **Acceleration is m/s², not g.** `imu.c` puts the QMI8658 driver in m/s² mode,
  so `amag` at rest is gravity, 9.807 — and `dyn_rms`/`dyn_peak` are
  `|a| − 9.80665`, in m/s². **Measured on this unit: the part reads 3.8 % high**
  (10.178 at rest), which is the sensor's own uncalibrated error, not the maths.
  So `dyn_rms` has a floor of about 0.37 and never reaches zero on a still board.
  Subtract a resting baseline before treating it as absolute.
- **Gyro is degrees per second**, temperatures are degrees C of the die, RSSI is
  dBm, and sound is dBFS (see above — not dB SPL).

## Verified serial output

A healthy boot, 2026-09-18, after the fixes (the two warnings are the ones
`CLAUDE.md` documents as expected on this board):

```
I (676) imu: QMI8658 up: WHO_AM_I 0x05, +-8 g, +-512 dps, sampling at 100 Hz (chip ODR 125 Hz)
I (687) sdcard: mounted at /sdcard in 55 ms: SL16G, 15193 MB, up to 10 files open at once
I (715) sensorous: recording to /sdcard/sensorous, 13492 of 15174 MB free
I (953) display: panel up: brightness init ESP_OK
I (5209) time: clock set from NTP (pool.ntp.org): 2026-09-18 07:10:27
I (7774) sound: ready: 16000 Hz mono, gain 30 dB, 200 ms bursts after a 50 ms settle. First reading -37.3 dBFS
I (7788) blescan: ready in 8 ms: passive scan, 4000 ms windows, 192 devices per window
I (13857) wifiscan: sweep 1: 23 APs in 2203 ms, 23 new, 23 known
I (17863) blescan: window 1: 24 devices in 4005 ms, 24 new, 24 known, 185 adverts total
I (22233) sensorous: heartbeat: mode stationary, 1 scans, wifi 23/512 ble 24/1024, records 2+49+1,
          card in 13492 MB, internal 30895 B (min 30379), psram 7882188 B, board 38.7 C,
          loop max 309 ms over 41 turns, drops 0
```

Steady state, stationary, on USB: 30.4 KB internal free and flat, 7.88 MB PSRAM
free, worst main-loop turn 339 ms (the 200 ms mic burst plus the record write),
Wi-Fi sweep 2.20 s, BLE window 4.01 s, 360 sensor records an hour.

## The stability run, 2026-09-18

40 minutes stationary on USB, ending 2,300 s of uptime. Every check in `vv.py`
passed on the files it produced.

| | |
|---|---|
| Uptime | monotonic across 66 heartbeats — no reset, no panic, no watchdog |
| Records | 239 sensor (357/hour against 360 expected), 38 scan cycles, 847 AP rows, 949 BLE rows |
| Internal heap | 30,351 B free, **drift 0 B** over an 11-minute window; sawtooth of 516 B per record |
| PSRAM | 7.88 MB free, drift 0 B |
| Main loop | worst turn 278–335 ms, bounded |
| Card | 13,492 MB free unchanged, 0 dropped log lines, 0 I/O errors |
| Temperature | 35.7–41.7 °C, against a 60 °C first guard step |
| IMU rate | median 101 Hz, one window in 239 at 75 Hz |

Two things the run showed that are worth knowing:

- **The tightest moment for internal RAM is a scan, not maintenance mode.** The
  low-water mark was 17,395 B, and it was set during a scan cycle in stationary
  mode — maintenance mode sits at a steady 19,175 B free. So the Wi-Fi sweep and
  the BLE window together are the worst case, and the headroom there is ~17 KB.
  Anything new that allocates internal RAM has to be measured against that number,
  not against the 30 KB the board shows at rest.
- **One Wi-Fi sweep in 59 came back empty** — 0 APs in 602 ms where every other
  sweep heard about 24 — around a maintenance-mode switch, so probably a scan cut
  short by the mode change rather than a spontaneous fault. It is written into
  `radio.jsonl` as an ordinary scan with no APs, which analysis cannot tell apart
  from a genuinely empty place. `vv.py` now fails a run that contains one. The
  firmware should log a warning when a sweep hears nothing; it does not yet.

# 06_sensorous

Measure everything this board can measure and write it to the microSD card:
the IMU, the ambient sound level, both die temperatures, every power rail, and
the whole radio environment — every Wi-Fi BSSID and every BLE address in
earshot, with RSSI.

Design note: [`docs/design/06_sensorous.md`](../../docs/design/06_sensorous.md).

**Status: first pass, compiles, never run.** The board was not available when
this was written, so nothing here has been flashed. Everything below about
runtime behaviour is intent until a boot log backs it.

## Build and flash

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/06_sensorous -B /tmp/ws-amoled-build/06_sensorous \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
idf.py -C projects/06_sensorous -B /tmp/ws-amoled-build/06_sensorous \
  -p /dev/cu.usbmodem3101 flash
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

## Verified serial output

Nothing yet. This section gets the first clean boot log, per repo convention.

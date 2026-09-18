# 06_sensorous — measure everything, write it down

*Design note, 2026-09-09. First run on hardware 2026-09-18.*
*Status: **M1 and M4 answered on hardware 2026-09-18.** What follows is what the*
*board does, except where it says otherwise.*

## What it is

The board sits somewhere — a shelf, a bag, a car — and writes down everything it
can measure about that place, forever, onto the microSD card.

```
  IMU ─┐
 temps ├──▶ [ sensors.jsonl ]
 power ┘
                                                    ┌─ pull the card
 Wi-Fi ─┐                                           │
   BLE ─┴──▶ [ radio.jsonl ] ──▶ microSD ──────▶ ───┤
                                                    └─ or download over Wi-Fi
                                                       in maintenance mode
```

It is the first project on this board with **no interaction as its purpose**.
02 was a picture book, 05 a dictation machine; both existed to answer a person.
This one exists to produce a file. The screen and the buttons are there so that
a person can tell it is working and take the card out safely — not to be used.

**Naming.** 03 was the voice remote, 04 the multilingual word book, 05 the
dictation experiment. This is **06**.

## What it measures

Everything the board actually has. Nothing is inferred and nothing is invented.

| Source | What comes out | Rate |
|---|---|---|
| **QMI8658 IMU** | accel x/y/z mean, \|a\| mean, dynamic RMS and peak; gyro mean and peak; pitch and roll; die temperature | sampled at 100 Hz, **summarised** per record |
| **ESP32-S3 die sensor** | chip temperature | 1 Hz, via `thermal.c` |
| **AXP2101 PMU** | battery mV and %, VBUS present and mV, VSYS mV, charge state, every rail's enable bit, latched IRQ bytes, PMU die temperature | per sensor record, plus its own `power.jsonl` |
| **Wi-Fi radio** | every AP heard: BSSID, RSSI, channel, auth, SSID | one sweep per scan cycle |
| **BLE radio** | every advertiser heard: address, address type, RSSI best and last, advert count, local name, company ID, TX power, connectable | one window per scan cycle |
| **ES8311 microphone** | ambient level only: RMS dBFS, peak dBFS, clipped-sample count | a 200 ms burst per sensor record; **the mic is closed the rest of the time** |
| **PCF85063 RTC** | the wall clock, kept across power cycles | at boot |
| **The firmware itself** | heap internal/PSRAM/minimum, card free space, log drops, rotations, deletions, worst loop turn | per sensor record |

**Not measured, because the board has none of it:** GNSS, cell, light, humidity,
pressure, magnetometer.

### The IMU is summarised, not sampled

A record every ten seconds carrying one instantaneous accelerometer reading says
nothing: the knock, the step and the door slam all happen between the samples.
So `imu.c` runs its own task at 100 Hz into an accumulator, and each record
carries the window's **mean, RMS and peak**. `a_dyn_rms` — the RMS of
(|a| − 1 g) — is the one number that answers "did anything happen here". Pitch
and roll come from the mean gravity vector and are only meaningful when
`a_dyn_rms` is small; the same record carries both, so a reader can tell.

## Location, precisely

The ask was "anything that could mark the location, so a Google API can use it
later". Here is exactly what that can and cannot be on this board.

**Wi-Fi BSSIDs are the location signal.** A BSSID is a globally unique address
bolted to a box that does not move. Google's Geolocation API takes a set of
`{macAddress, signalStrength, channel, age}` and returns a lat/long with an
accuracy radius. So the `ap` record uses **Google's own field names verbatim** —
a host-side script groups the lines by `seq` and posts them with no field
mapping at all. `tools/locate.py` is that script.

**BLE addresses are mostly not.** Modern phones, watches and earbuds advertise a
Resolvable Private Address that rotates every ~15 minutes. Those rows will never
be seen again and are worthless for placing anything. What is worth keeping:
`addr_type` **0 (public)** and **1 (random static)** — beacons, fixed equipment,
some fitness sensors. Every BLE row records its address type precisely so
analysis can throw the rest away, and both the census dashboard and this note say
so rather than letting someone discover it at analysis time.

The BLE rows still earn their place: **advert volume is an occupancy signal.**
Forty rotating addresses means a room with people in it, whatever their MACs say.

**What is not there:** no GNSS, no cell towers, no way to check a fix. Every
position this data can produce comes from Google's database of other people's
BSSIDs, with whatever error that carries.

**Nothing is sent from the board.** The board collects; `locate.py` on a laptop
is the only thing that talks to Google, only when given `--key`, and only the
four fields above — never SSIDs, never BLE, never a whole file.

## Sound: a level, and only a level

The microphone measures how loud the room is. It does not, and cannot in this
build, do anything else.

This is enforced by the shape of the code rather than asserted in a comment:

- **The mic is closed between measurements.** `esp_codec_dev_close()` runs at the
  end of every burst. Between bursts the ES8311's ADC is not running and the I²S
  channel is not clocked, so nothing is being captured — not into a DMA ring, not
  anywhere. At the stationary cadence the board listens for 200 ms out of every
  10 s and is deaf for the other 9.8.
- **Audio never leaves `sound.c`.** Samples are read in 512-sample chunks into one
  small static buffer, folded into four running sums, and overwritten by the next
  chunk. Nothing is concatenated and nothing is kept; the largest quantity of
  audio in existence at any instant is 1 KB that no other module can reach, and
  it is zeroed when the burst ends.
- **Four numbers come out**, and they are all that is written or served:
  `rms_dbfs`, `peak_dbfs`, `clipped`, `n`. There is no VAD, no recogniser, no
  spectrum, and no code path in this image that could turn a level into content.

The first 50 ms of each burst is discarded: the ES8311's ADC has a start-up
transient after every open, and measuring it would measure the codec, not the
room. The DC offset is subtracted before the RMS, because a MEMS mic through a
codec sits on a bias and an RMS that includes it is a measurement of the bias.

**dBFS is not dB SPL.** 0 dBFS is a full-scale square wave, so a full-scale sine
reads −3.0. It is an uncalibrated electrical level behind 30 dB of gain. Two
readings from this board compare with each other; neither compares with a sound
level meter until someone does a calibration. The screen prints the unit for
exactly this reason. `clipped` says when the reading is a floor rather than a
value.

## Two modes, one switch

| | **stationary** | **mobile** |
|---|---|---|
| assumes | wall power, a fixed place | battery, movement |
| sensor record | every 10 s | every 1 s |
| scan cycle | every 60 s | every 10 s |
| power record | every 60 s | every 30 s |
| a fix is | representative of the whole run | per cycle, and it matters |

Switched by a **short press on BOOT**, by `M` on the serial console, and it is
named on the screen and in every record. Long press is maintenance mode, as in
02 and 05.

There is deliberately no multi-press gesture — CLAUDE.md's rule, earned on
2026-09-06, and it stands.

## The radios never scan at the same time

One antenna, one 2.4 GHz front end. A Wi-Fi channel sweep and a BLE listening
window overlapping means each gets a fraction of the airtime it asked for and
neither result means anything — worse, it means *quietly* less, which is the kind
of wrong that survives into a dataset.

So a scan cycle is strictly sequential, inside one task:

```
  wait (period, or a kick) ─▶ Wi-Fi sweep ~2 s ─▶ BLE window 4 s ─▶ back to wait
```

That task exists so the sweep can block for seconds while the sense loop keeps
answering the button and redrawing the screen.

**Both stacks stay resident** for the whole run. Bringing Wi-Fi up and down per
cycle costs seconds, and CLAUDE.md already records that `esp_wifi_deinit()` does
not give the internal RAM back anyway. `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` is on
because both are initialised, even though they are never used at once.

**Maintenance mode stops scanning.** The radio cannot sweep channels and hold an
association at the same time, and a sweep during a download would stall it. The
screen says so while it is up.

## The card is the product

Five files under `/sdcard/sensorous/`:

| File | Records | What it is for |
|---|---|---|
| `sensorous.log` | — | the flight recorder: every `ESP_LOG` line, mirrored |
| `sensors.jsonl` | `sensor` | the periodic measurement: IMU, sound level, temperatures, power, heap, card |
| `radio.jsonl` | `scan`, `ap`, `ble`, `scan_end`, `census_*` | the environment |
| `power.jsonl` | `power` | rails, battery, VBUS |
| `events.jsonl` | `boot`, `event` | why anything changed |

### One line per sighting, not one array per scan

A dense scan is 30 APs and 100 BLE devices. As a single JSON object that is
~15 KB — **larger than the 32 KB ring that carries it once two of them queue**,
so a busy moment would drop the entire record rather than one row of it. Small
lines also survive a card pulled mid-write: the file ends at a line boundary and
everything before it parses. `locate.py` reassembles by `seq` in four lines of
Python, and its test against a deliberately truncated file passes.

### Rotation, retention, and running out of card

A card that fills silently stops the log without stopping the board. That is the
failure that looks like everything is fine, so it is handled explicitly:

- each file rotates at its cap (log 4 MB, data 8 MB) to `<name>.1`, older
  generations shifting up to `CONFIG_SENSOROUS_KEEP_FILES` (8);
- every ten seconds `sdlog_maintain()` checks the **tracked** sizes — no `stat()`
  on the card, which costs tens of milliseconds;
- every five minutes it re-reads free space, and below 64 MB it deletes the
  oldest generation, **loudly**, and says which file and how big;
- `rotations` and `deletions` are in every sensor record and on the dashboard.

Nothing else ever deletes anything.

### Ejecting

There is no card-detect pin on this board and no mechanical interlock. So:

- **EJECT on the screen** (single tap, bottom right) — flush, close, unmount,
  and the button becomes MOUNT;
- `e` / `o` on the serial console;
- `POST /api/eject` and `/api/mount` from the dashboard.

All three route to the same place, and the LVGL callback only *latches* the tap —
the main loop owns the card, because the LVGL task must never touch it.

A card pulled **without** an eject is detected by the presence poll within five
seconds, the files are closed, and `events.jsonl` records
`"card": "removed without an eject"`. Whatever had not drained is gone, and the
screen says `NO CARD - nothing is being saved` in red.

## Export over Wi-Fi

Maintenance mode joins the home network and serves the card:

```
GET  /                one page: metrics, files, census, log tail, buttons
GET  /api/files       every file with its size
GET  /api/file?name=  one file, whole, streamed. `from=` resumes at a byte offset
GET  /api/census?kind=wifi|ble    the address tables straight out of PSRAM
GET  /api/metrics /api/state /api/log
POST /api/eject /api/mount /api/dump /api/reboot
```

`from=` is what makes a 60 MB file recoverable over a flaky link: ask again from
where it stopped instead of starting over. `send_wait_timeout` is 30 s, not the
default 5, because a whole-file download over SDMMC 1-bit takes minutes and a
timeout partway leaves a truncated file that looks complete.

`name=` is validated to a plain basename in the data directory — no separators,
no dot-dot. The board serves files; it must not serve the filesystem.

**STA, not SoftAP.** 02 tried AP mode on 2026-09-05, the phone side did not work
out, and it was removed the same day. STA is the path that works here.

## No SoftAP, no cloud, no upload

The board never initiates a connection to anything but NTP. `locate.py` is the
only thing that talks to Google, from a laptop, when asked — and it sends four
fields per access point, never SSIDs, never Bluetooth, never sound, never a file.

## Milestones

| | What | State |
|---|---|---|
| **M0** | Scaffold: carried modules wired, builds clean, 240 MHz, BT+Wi-Fi resident | **done** — 1.58 MB image, 62 % of the app partition free |
| **M1** | First boot: does it run? IMU answers, the mic burst returns a sane level, both radios scan, records land on the card | **done** 2026-09-18 — after six fixes. All four answer yes; `tools/vv.py` checks them from the files |
| **M2** | Memory and timing: internal RAM with both stacks up, worst loop turn, sweep and window durations, records per hour | **measured** 2026-09-18, not yet analysed: 30.4 KB internal free (flat over 8 min), 7.88 MB PSRAM, worst loop turn 339 ms, Wi-Fi sweep 2.20 s, BLE window 4.01 s, 360 sensor records/hour stationary |
| **M3** | The card under stress: rotation at the cap, retention deleting, eject/reinsert live, a card pulled without an eject | not started |
| **M4** | Export: a whole file over HTTP, resumed download, `locate.py` against a real `radio.jsonl` with a key | **mostly done** 2026-09-18 — 735 KB of all five files over HTTP, and resumed downloads, via `tools/extract.sh`. `locate.py` against Google with a key is still untested |
| **M5** | A real run: a night stationary, then carried, both on battery. Drain, drift, and whether the fixes agree with where it was | not started |

## Open questions, honestly

- **Internal RAM with Wi-Fi and NimBLE both resident.** 05 measured 166 → 110 KB
  free just for Wi-Fi. NimBLE observer-only is perhaps another 40 KB. There is no
  ESP-SR here, which is where the room comes from, but the number is unmeasured.
  M2 answers it; if it is tight, BLE becomes the thing that gets torn down.
- **Whether a 100 Hz IMU task on the shared I²C bus disturbs anything.** The PMU,
  the RTC and the CST820 touch controller are on the same two wires. The die
  temperature read is already throttled to 1 Hz for this reason; the 100 Hz
  six-axis read is not.
- **Whether opening and closing the ES8311 every cadence period is reliable.**
  It is the design that makes "not listening between measurements" structural
  rather than a promise, and `esp_codec_dev_open`/`close` is meant to be called
  repeatedly. But nothing in this repo has cycled it thousands of times, and the
  burst duration is recorded (`sound.ms`) so a drift or a stall shows up. If it
  proves flaky the fallback is to hold it open — which is worse, and would have
  to be said plainly here.
- **What the resting level actually reads.** 30 dB of gain is 02's working value
  for speech recognition, not for a level meter. If quiet rooms clip, the gain
  comes down; `clipped` in every record is how that gets noticed.
- **Battery life in mobile mode.** Scanning every 10 s with both radios up is the
  most radio-active thing this repo has built. 02 measured a paused recogniser
  taking a battery from 95 % to 46 % in 72 minutes; this could be worse.
- **How fast the card actually fills.** A stationary run at 60 s cycles in a
  suburb is perhaps 40 lines per cycle — roughly 8 MB/day. Mobile at 10 s in a
  city could be 20× that. The caps and retention exist because this is a guess.
- **Whether the census tables are big enough.** 512 Wi-Fi and 1024 BLE. A walk
  through a shopping centre will overflow the BLE table with rotating addresses
  in under an hour. `census_overflow()` is recorded so the first run says so.

## What this reuses

Almost all of it. That was the point of asking.

| From | What |
|---|---|
| `05_dictation` | `sdlog` flight recorder and record channels, `sdcard`, `pmu`, `thermal`, `timesync`, `pcf85063`, `button`, `devcmd`, `maint` server, `display` panel control |
| `02_word_book_en` (via 05) | the panel bring-up that avoids `bsp_display_start()`'s PSRAM draw buffer, the checked brightness write, the boot-streak safe mode |
| `ws-ESP32-C6-Touch-AMOLED-1.8/18_govee_monitor` | the NimBLE observer shape — sync callback, host task, GAP discovery, little-endian address reversal |
| `ws-ESP32-C6-Touch-AMOLED-1.8/14_sensory_play` | that `waveshare/qmi8658` is the right component and takes the BSP's I²C bus handle |
| `05_dictation` (`audio_io.c`) | the I²S + ES8311 bring-up `sound.c` is built on — 16 kHz mono, verified on this board |
| new here | `census`, `wifiscan`, `blescan`, `imu`, `sound`, `srec`, the log management in `sdlog`, eject in `sdcard`, the status screen, the export routes, `tools/locate.py` |

One structural change to a carried module: `wifi_sta.c` now owns the driver
lifecycle (`wifi_radio_up` / `wifi_radio_down`) rather than initialising and
deinitialising around a single join, because scanning needs the driver resident
and maintenance mode needs to associate on the same driver.

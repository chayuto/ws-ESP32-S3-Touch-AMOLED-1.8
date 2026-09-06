# 02_word_book_en: what else the board can tell us about itself, safely

Written 2026-09-06. The question was: what other telemetry can the firmware log while the
board is active without disturbing it? The thermal part below was built the same day (the
user asked for temperature logging and an emergency thermal shutdown, for child safety);
the rest is a ranked list waiting for a go. Companion to `02_word_book_en_memory.md`.

## What "safely" means here

Every candidate was scored against these rules:

1. **Read-only against the hardware.** No register write that changes a rail, a gain or
   a latch. The exceptions are the thermal guard's own actions (charger off, soft
   power-off), the PMU's thermal-throttle line, and turning on a sensor that is off
   today (the accelerometer). All reversible.
2. **No new task, no new timer.** Reads happen on the main task at record time (every
   30 s awake, 300 s asleep, and on events) or inside the detect task's per-frame loop.
3. **Bounded time on the main task.** Under 5 ms per record all in, so the 250 ms loop
   keeps its button, dedupe and card timing. A one-byte AXP2101 register read costs about
   70 µs at 400 kHz; today's state record spends about 1.3 ms on the I2C bus.
4. **Nothing blocks on the card from the main task.** Records go through the PSRAM ring
   and the drain task as they do now. The one slow card query (free space) runs once at
   mount.
5. **Records grow, the log does not.** Values go in the JSON line; a log line only when
   something changes.
6. **Never audio, never a secret.** Utterance capture stays parked.

## Already recorded (nothing below duplicates it)

`state`: USB and VBUS mV, battery present/mV/%/charge phase, VSYS, rail-enable registers,
screen state and brightness, asleep/maint flags, internal heap now and minimum, PSRAM,
card/files/log flags and log size, word count, heard count with the last word and its
confidence, mic average/peak dBFS and speech %, and (from 2026-09-06 afternoon) the two
die temperatures and the thermal level. `boot`: reset reason, RTC RAM kept/lost, PMU
power-on and power-off sources, crash streak, and now die temperatures, charger settings
and the last thermal trip. `input`: every press, tap and command with what it landed on
and what it did. `classifier`: every detection with its candidates and verdict.

## Built: temperature and the thermal guard

Two dies report their temperature for the price of three register reads a second:

| Sensor | How | Cost | Notes |
|---|---|---|---|
| ESP32-S3 die | `driver/temperature_sensor`, range 20-100 °C (±2 °C) | ~20 µs | Power is reference-counted with the Wi-Fi PHY in IDF 5.5 (`sar_periph_ctrl_common.c`), so maintenance mode does not fight it |
| AXP2101 die | REG 3C/3D, `22 + (7274 - raw) / 20` (the vendor library's formula); the ADC channel was already enabled by our `0x1D` write | 2 reads, ~150 µs | The charger is the biggest heater on the board, and this die sits in it |

Measured at boot on USB, 2026-09-06 14:21, room about 22 °C: chip 30.7 °C, PMU 34.8 °C.
Steady state after the first minutes listening at 85 % brightness: chip ~34-36 °C, PMU
~36 °C.

The guard (`thermal.c` decides, `app_main.c` acts) takes the hotter die as "the board":

| Level | Default (die °C) | Action |
|---|---|---|
| warm | 60 | Screen held at the dim level, status line says "warm - screen dimmed to cool" |
| hot | 70 | Sleep (screen off, recogniser stopped), charger off, maintenance ended if on; BOOT shows a dim "too warm - cooling down, wait" for three seconds and nothing else |
| trip | 80 | Marker to NVS, `thermal_trip` record, three seconds for the card to sync, then PMU soft power-off (REG 10 bit 0). PWR or a USB insert brings it back; the boot record then carries `last_trip` and the PMU reports the power-off as `software` |

A level changes only after five consecutive one-second samples agree, and drops only
5 °C below the line it crossed. Leaving hot turns the charger back on and leaves the
board asleep, so BOOT wakes it like any sleep. A board still at the trip line at boot,
on two readings a second apart, goes straight back off before the screen and the
recogniser add to it. A reading outside -20..130 °C is ignored; if the two dies disagree
by more than 25 °C a warning names the suspect once, and the hotter one still rules.

Two things the PMU does on its own, set at every boot: the charger's thermal throttle
(REG 65) moved from the chip default 100 °C to `CONFIG_WORDBOOK_PMU_TREG_C` (60 °C), so
the PMU lowers its own charge current past that line whether or not the firmware is
running; and charging is switched back on if a trip left it off (REG 18 survives a soft
power-off). The charger as found on this board: input limit 1500 mA, charge 200 mA to
4.20 V. 200 mA is gentle for any cell that fits this board; it is also why charging is
slow.

Verified 2026-09-06 14:23 with the serial `t` simulation (which feeds a made-up board
temperature; a simulated trip is a dry run): ok → warm dimmed the screen; warm → hot
switched the charger off and slept the board; `s` while hot was ignored (`ignored_hot`);
hot → trip logged the trip without writing or switching anything; simulation off →
trip → ok after five seconds and the charger came back on; `s` then woke it, front end
rebuilt in 177 ms. Not exercised: a real trip (needs a real 80 °C die, or a hand on PWR
afterwards) and the BOOT press while hot (needs a hand).

**Calibration is still owed.** The lines are die temperatures. The glass and the plastic
run cooler than the dies, by an amount nobody has measured on this board. Before the
child test: a thermometer on the glass after twenty minutes charging at full brightness,
against the `pmu_c`/`chip_c` in the state records, then set the lines so that "warm"
means warm to a hand.

## Ranked: what else, cheaply

### Tier A: free and safe, one round, no new hardware touched

| # | Item | Fields | What it answers | Cost |
|---|---|---|---|---|
| 1 | AFE backlog and detect-loop health, from the fetch result the detect task already holds (`ringbuff_free_pct`, fetch timeouts, mic read failures, `detect()` duration, event-queue drops) | `afe_busy_max`, `afe_timeouts`, `mic_errors`, `detect_ms_max`, `q_drops` | Did we drop audio while the child spoke; is MultiNet keeping up with 20 words. The missing-word question has no data today | two timestamps per 32 ms frame |
| 2 | Main-loop stall | `loop_max_ms`, `loop_turns` | Did a JPEG cache, a card mount attempt or a maintenance join freeze the loop (button latency, dedupe) | one timestamp per turn |
| 3 | Heap shape | `internal_largest`, `psram_min` | Fragmentation: the front-end rebuild on wake needs 37 KB of contiguous internal RAM | µs |
| 4 | Stack headroom for main, `sr_feed`, `sr_detect`, `taskLVGL`, `sdlog`, `devcmd` (`uxTaskGetStackHighWaterMark`, handles by name) | `stack_min` as one compact string | An overflow is a panic today, with no warning before | ~50 µs |
| 5 | Log pipeline | `log_dropped`, `io_errors`, `state_bytes` | Are records being lost before the card | 0 |
| 6 | Host vs charger (`usb_serial_jtag_is_connected`) | `host` | On the Mac or on a wall charger; explains dropped console lines | 0 |
| 7 | PMU read-only extras: IRQ status REG 48/49/4A as three hex bytes, read and never cleared (latched since the PMU last lost power: PWR short/long press, VBUS in/out, charge start/done, gauge warning levels, die over-temperature, charger safety timer); low-battery warning levels REG 1A (chip defaults 1 % and 15 %); TS pin ADC REG 36/37 once at boot (tells whether an NTC is fitted) | `pmu_irq`, boot-only fields | Did anyone press PWR; did the gauge warn; is there a battery thermistor | 8 reads, ~0.6 ms |
| 8 | Wi-Fi in maintenance (`esp_wifi_sta_get_ap_info`) | `rssi`, `channel`, join ms in the `maint_in` record and `/api/metrics` | Slow uploads, failed joins | 0 |
| 9 | RTC quality in the boot record: oscillator-stop flag (already logged as text) and RTC-vs-NTP delta at the boot sync | `rtc_valid`, `rtc_drift_s` | Does the PCF85063 hold time through a power loss, and how well | 0 |
| 10 | Card free space once per mount (`esp_vfs_fat_info`; FSINFO is trusted, `CONFIG_FATFS_DONT_TRUST_FREE_CLUSTER_CNT=0`, so usually fast; a stale FSINFO scans the FAT, which is why it is never periodic) | `card_free_mb` in the `card_in`/`boot` record | When will the log rotation matter | once |

Together: about +170 bytes per state record (333 today), under 1 ms more on the bus,
buffer 768 → 1024. State file growth ~1.0 → ~1.5 MB per day awake; rotation stays at
4 MB on open.

### Tier B: needs a go, a config change or a new device on the bus

| # | Item | Fields | Why it waits |
|---|---|---|---|
| 11 | CPU load per core: `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` (64-bit counter), then `ulTaskGetIdleRunTimeCounterForCore(0/1)` deltas | `cpu0_pct`, `cpu1_pct` | Kernel config change; a timestamp per context switch, under 1 % CPU. Answers how much headroom there is for `03_word_book_multi` and for sound plus recognition together |
| 12 | IMU, QMI8658C at 0x6b: accelerometer only at a low-power rate, gyro off; 8-byte burst (temperature + XYZ) per record and a 4 Hz sample from the loop for a "moved" flag | `orient` (face_up/face_down/upright/held), `tilt_deg`, `moved`, `imu_c` | A new device on the shared bus (touch, PMU, RTC, codec), ~0.3 ms per record, tens of µA. Its INT1 lands on the TCA9554's EXIO6 (schematic), not on an S3 GPIO, so motion means polling either way. Now: was the board face-down (mic against the table) or being carried when words were missed; a third temperature point. Later: pick-up wakes instead of BOOT. Driver: `waveshare/qmi8658` ^2.0.0 from the registry (the vendor's `92_qmi8658_imu` uses it) or 60 lines of our own |

### Not available or not worth it

- Battery current: the AXP2101 has no current ADC. Drain is the `vbat_mv`/`pct` slope
  in the state file, which is already there.
- Panel and touch: CO5300 and CST820 report nothing beyond what we set.
- ES8311: gain readback is a static setting.
- LVGL render time: LVGL 9's performance monitor draws on the screen; no silent counter
  worth the coupling.
- TCA9554 inputs: EXIO3 is `RTC_INT`, EXIO6 is `QMI_INT1`; only useful once alarms or
  motion engines are configured.

### Safety aside, not telemetry

The task watchdog watches only the idle tasks (5 s). A wedged main loop would hang
silently, which is the shape of the 2026-09-06 13:00 scare even though that one was not
a hang. Subscribing the main task with the timeout raised to 30 s (card JPEG caching and
the maintenance join run on it) would turn a wedge into a `task_wdt` reset that the boot
record already names. Separate decision.

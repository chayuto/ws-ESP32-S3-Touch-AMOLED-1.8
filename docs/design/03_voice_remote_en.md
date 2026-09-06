# 03_voice_remote_en — the board as a voice remote for a Pico2Go robot

*Design note, 2026-09-06. Status: proposed, not started. One decision gates M1 — see*
*[The link](#the-link-and-why-the-obvious-plan-does-not-work).*

## What it is

You say a word at the AMOLED board. A robot on the floor does it.

```
   "turn left"          BLE advertisement              wheels
  ──────────────▶  [ S3 AMOLED board ]  ~~~~~~~~~▶  [ Pico2Go ]  ──────▶
      voice            recogniser +                   observer
                       transmitter                    + motors
```

Six commands: **go, back, left, right, spin, stop.**

The board is the remote, not the robot. That is the whole architectural idea and it is
worth stating plainly, because the alternative — a mic on the robot — is much worse:

- The mic sits next to two N20 gearmotors. Motor noise is broadband and *co-located with
  the microphone*, which is the one thing a single-mic AFE cannot fix.
- The robot moves away from the speaker; the board stays on a table at a known distance.
- The board already has a screen, so every command can be *shown* as it is sent. A
  toddler pointing a robot at a wall needs to see that the board heard "left".

It also means project 3 is mostly project 2 with a different action behind the same
event. See [Reuse](#reuse-from-02).

**Naming.** `03_word_book_multi` was pencilled in as project 3 in the 02 note. This
displaces it: the robot becomes **03**, the multilingual word book becomes **04**. The
DTW-recogniser argument in 02's *What project 3 needs* section still stands, it just
moves up one number.

## The target hardware, audited

Waveshare **Pico2Go** (SKU 31697/31698), kit includes an **RP2350-Plus** control board.
Audited 2026-09-06 from the wiki, the RP2350-Plus spec page, and the demo code the
Pico2Go wiki itself links (`PicoGo_Code_V2.zip` — Waveshare reuse the PicoGo code and
chassis; check `PicoGo_Schematic_V2.pdf` before committing to a pin).

| | |
|---|---|
| Controller | RP2350-Plus: RP2350A (dual M33 / dual Hazard3, 150 MHz), 520 KB SRAM, 4 MB flash, USB-C, MP28164 buck-boost, Li-po header |
| **Radio** | **none** |
| Motors | 2 × N20 metal-gear, TB6612FNG driver |
| Sensors | 5-ch analog line follower, HC-SR04 ultrasonic, 2 × IR obstacle, battery ADC |
| Output | 1.14" IPS 240×135 ST7789, 4 × WS2812, buzzer |
| Remote | IR receiver + IR remote in the box; **JDY-33** Bluetooth module for a phone app |
| Power | 2 × 14500 Li-ion (not included), protection + charge-while-run |
| Header | standard Raspberry Pi Pico; product page says Pico 2 / 2 W / Pico W all fit |

Pin map from the demo code:

| GP | Function | GP | Function |
|---|---|---|---|
| 0 / 1 | **UART0 → JDY-33** | 14 / 15 | ultrasonic Trig / Echo |
| 2 / 3 | IR obstacle right / left | 16–21 | TB6612: PWMA, AIN2, AIN1, BIN1, BIN2, PWMB |
| 4 | buzzer | 22 | WS2812 |
| 5 | **IR receiver** | 25 | LED |
| 6 / 7 / 27 / 28 | line sensor clk / addr / data / cs | 26 | ADC0 battery |
| 8–13 | ST7789: DC, CS, SCK, MOSI, RST, BL | | |

GP0–22 and GP26–28 are all claimed. There is no spare bus on that header.

## The link, and why the obvious plan does not work

The plan was: board emits a connectionless BLE advertisement, robot picks it up, no
pairing. **The robot as shipped cannot do that**, for two independent reasons:

1. **The RP2350-Plus has no radio.** It is an enhanced Pico 2, and the plain Pico 2 has
   no radio either. Waveshare's wireless part is a *different* board — the RP2350B-Plus-W
   (RP2350B + Radio Module 2, Wi-Fi 4 + BT 5.2, 16 MB flash). Not in this kit.
2. **The JDY-33 is a transparent UART bridge in peripheral role.** BT 3.0 SPP + BLE, AT
   commands limited to name / baud / sleep / disconnect. It advertises and waits to be
   connected to. It has no observer role: it cannot scan, and cannot parse manufacturer
   data out of somebody else's advertisement. The Waveshare wiki even warns its BLE side
   fails with their own app ("select JDY-33-SPP; if you select JDY-33-BLE the connection
   will fail").

Nothing on that robot can hear a broadcast. Four ways out:

| | Approach | Robot changes | Verdict |
|---|---|---|---|
| **A** | **Swap RP2350-Plus → Raspberry Pi Pico 2 W** | ~$7 board, two line fixes | **plan of record** |
| B | S3 as BLE *central*, connects to the JDY-33's BLE serial and writes the app's JSON | none | fallback; a connection, not a broadcast, and the wiki flags that path as flaky |
| C | S3 drives an IR LED; robot uses its existing IR receiver | **none at all** | good phase 0; needs an IR LED soldered to the AMOLED board |
| D | ESP-NOW | — | **impossible** — Espressif-proprietary; the Pico's CYW43439 cannot speak it |

**Option A is the plan.** A Pico 2 W is RP2350 + CYW43439 with BLE 5.2 and a real stack
(BTstack in the Pico SDK, `aioble` in MicroPython). It can be a **passive observer**:
scan advertisements, parse manufacturer data, never connect, never pair. That is the
original architecture, unchanged, and everything else on the robot is untouched. The
JDY-33 simply goes unused, or stays for the phone app.

Two things to fix on the swap: on a Pico 2 W the onboard LED lives on the CYW43, not
GP25 (`bluetooth.py:31`), and the CYW43 claims GP23/24/29 — none of which the robot uses.

**Option D is settled and closed.** ESP-NOW was raised as possibly cheaper than BLE; with
this robot it cannot work. BLE it is.

**Option C is worth keeping warm.** The robot already has an IR receiver on GP5 and the
shipped `IRremote.py` maps remote keys 2/8/4/6/5 to *forward, back, left, right, stop* —
exactly our six minus spin. Driving NEC codes from the S3's RMT peripheral changes
**nothing** on the robot, which makes it the fastest possible proof that the voice half
works. The catch is physical: the AMOLED board's free pins are mostly brought out to FPC
pads rather than a header, so an IR LED means soldering to a small round board, and IR
needs line of sight.

**Decision required before M1: buy a Pico 2 W, or plan around option B.** Everything
below assumes A.

## Architecture

```
        S3 AMOLED board (transmitter)                    Pico 2 W (receiver)
 ┌────────────────────────────────────────┐      ┌──────────────────────────────┐
 │ core 1                                 │      │  BLE observer (passive scan) │
 │   audio_task → AFE(VAD) → MultiNet7    │      │    parse mfg data            │
 │   → word_event{id, prob, text} ────┐   │      │    check magic + crc         │
 │                                    │   │      │    ignore repeat seq         │
 │ core 0                             ▼   │  ~~▶ │       │                      │
 │   on_command():                        │ adv  │       ▼                      │
 │     ├─ draw arrow + word on screen     │      │  drive(cmd, arg) + TTL timer │
 │     ├─ input/classifier records → SD   │      │    → TB6612 → N20 motors     │
 │     └─ ble_tx_command(cmd, arg, ttl)   │      │  TTL expires → stop()        │
 └────────────────────────────────────────┘      └──────────────────────────────┘
```

The transmitter is stateless per command. The receiver holds exactly one piece of state:
*what am I doing, and until when.*

## Recognition

Same engine, same code path as 02. `recognizer_start(mic, words, count, queue)` takes a
different `word_def_t[]` and nothing else changes — the recogniser was written for this
(`recognizer.h`: *"the one module the multilingual follow-on replaces"*).

**The problem with these six words is that they are all one syllable.** That is the worst
case for MultiNet7, and we already have evidence in 02: BALL/CAR and THREE/SEVEN confuse
each other, and quiet-room false positives are still open. `GO` is the worst of the set —
two phonemes, G‑OW.

Two mitigations, both free:

**Phrase variants.** ESP-SR keeps commands in a singly linked list of phrases each
carrying a `command_id`, so several phrases can map to one id:

| id | command | phrases |
|---|---|---|
| 0 | STOP | `STOP` |
| 1 | GO | `GO FORWARD`, `FORWARD`, `GO` |
| 2 | BACK | `GO BACK`, `BACKWARD`, `BACK` |
| 3 | LEFT | `TURN LEFT`, `LEFT` |
| 4 | RIGHT | `TURN RIGHT`, `RIGHT` |
| 5 | SPIN | `SPIN AROUND`, `SPIN` |

More phonemes means better separation. `apply_words()` currently uses the array index as
the id, so `word_def_t` needs an explicit `id` field — a small change, and one 04 wants
anyway. `esp_mn_commands_phoneme_add(id, string, phonemes)` is there if G2P mispronounces
something and we need to hand-tune.

**Per-command confidence floors.** Already on 02's backlog as tuning; here it is a safety
feature and it is *asymmetric on purpose*:

| | floor | reasoning |
|---|---|---|
| STOP | **low** | must be easy to stop. A false STOP costs nothing. |
| LEFT / RIGHT / SPIN | medium | turns in place, low consequence |
| GO / BACK | **high** | the robot travels. A false GO is the failure that matters. |

## The wire protocol

Legacy BLE advertisement, manufacturer-specific data (AD type 0xFF). Broadcaster role
only: no connection, no GATT, no bonding, no pairing dialog anywhere.

```
 [0..1]  company id  0xFFFF   (the "for testing" range)
 [2..3]  magic       'R','C'
 [4]     version     1
 [5]     seq         ++ per new command; receiver ignores a repeat
 [6]     cmd         0=stop 1=go 2=back 3=left 4=right 5=spin
 [7]     arg         speed 0..100, or degrees for spin
 [8]     ttl         dead-man, in units of 100 ms
 [9]     prob        confidence × 255, so the robot may apply its own floor
 [10..11] crc16
```

14 bytes with the company id — comfortably inside the 31-byte legacy payload, with room
for a short local name.

Each new command is **burst**: advertise at a 20–30 ms interval for ~300 ms, then fall
back to an idle beacon or stop advertising. Adverts are unacknowledged and get lost; a
burst plus a sequence number gives redundancy without the receiver acting twice.

Both halves have precedent in the C6 repo — `16_bitchat_relay/main/ble_relay.c`
advertises with NimBLE, and `18_govee_monitor/main/ble_scanner.c` is a NimBLE observer
parsing manufacturer data out of unconnected adverts. That is exactly the receive side,
already written once in this workspace family.

## Safety

A false positive in 02 shows the wrong photo. A false positive here drives a robot into
the stairs. Three things are non-negotiable and go in from the start:

1. **The dead-man lives in the packet.** Every command carries a TTL; the receiver stops
   on its own if the next advert does not arrive. Board out of range → robot stops. Board
   loses power → robot stops. Board crashes → robot stops. This is the important one,
   because it fails safe without either side noticing anything is wrong.
2. **STOP is asymmetric** — lowest confidence floor of the six, and it pre-empts whatever
   is running.
3. **A moving robot needs a shorter TTL than a turning one.** GO/BACK get a short TTL
   (~1 s) so a lost burst stops the robot quickly; a spin can afford longer.

Worth noting and then moving on: no pairing means no authentication. Anyone in range can
spoof a command. For a toy that is acceptable; if it ever matters, a shared secret plus
the existing rolling counter is cheap.

The shipped `bluetooth.py` gets this right in its own way — the phone app sends
`{"Forward":"Down"}` on press and `{"Forward":"Up"}` on release, so the dead-man is the
button release. A voice command has no release, which is precisely why we supply the
timeout ourselves.

## Budget — the S3 side

The robot side is not tight: a Pico 2 W has 520 KB SRAM and does one job. The board side
is, because 02 already fills it.

| Resource | Now (02, measured) | After | Risk |
|---|---|---|---|
| Internal RAM | **~56 KB free** at idle (`internal=55851`) | BLE controller + NimBLE host take a real bite | **highest — measure at M1 before writing anything else** |
| App flash | 4 MB partition, **7 % free** | NimBLE adds ~150–250 KB | grow `factory` 4M → 8M; 5 MB is unallocated and `02_word_book_en_memory.md` already flags this as owed |
| PSRAM | 4.1 MB free | unchanged | none |
| CPU | MultiNet 18–28 ms of each 32 ms frame on core 1 | NimBLE host task on core 0 with LVGL | pin it explicitly |
| Radio | Wi-Fi STA in maintenance mode only | one 2.4 GHz radio, software coexistence | make maintenance mode and robot mode mutually exclusive — you are not uploading photos while driving |

Trim NimBLE hard: broadcaster role only, `MAX_CONNECTIONS` 0/1, no security, no observer,
no GATT. `18_govee_monitor/sdkconfig.defaults` is a good template for how far it goes
(it disables every role but the one it needs).

## Reuse from 02

Verbatim, no changes: `recognizer.c`, `audio_io.c`, `cards.c`, `sdlog.c`, `clog.c`,
`thermal.c`, `pmu.c`, `button.c`, `maint.c`, the state/classifier/input records, the
flight recorder, safe mode, the serial command set.

Replaced: `on_word()` becomes `on_command()` — send a packet and draw an arrow instead of
loading a photo. New: one `ble_tx.c`.

Dropped: the SD photo/book layer. The vocabulary is compiled in — six commands is not
content, and a robot whose command set can be changed by dropping files on a card is a
robot that can be broken by dropping files on a card.

## Risks, ranked

1. **A false GO with the robot on the floor.** The whole safety story. Mitigated by the
   TTL dead-man, the asymmetric floors, and M4 measuring the actual rate in a real room.
   Never resolved by argument — only by M4.
2. **One-syllable confusability.** Variants, per-command floors, `phoneme_add` if needed.
   Partly measurable at M3 with an adult; really settled at M5.
3. **Internal RAM on the S3.** ~56 KB free and BLE wants a chunk of it. This is M1's
   go/no-go, exactly as MultiNet-without-WakeNet was 02's M1.
4. **Advert loss and range.** Bursting plus sequence numbers; the TTL turns a lost burst
   into a stop rather than a runaway.
5. **Latency.** Voice → motion. MultiNet decides at end of utterance, then the first
   advert lands within ~20–30 ms. Expect 100–300 ms end to end: fine for a toy, not for
   precision driving. Measured at M4, not estimated.
6. **Radio coexistence** with maintenance-mode Wi-Fi. Mitigated by making the modes
   exclusive.
7. **The Pico 2 W swap is unverified.** The product page claims compatibility; nobody has
   put one in this chassis yet. M0 exists to prove it before anything is built on it.

## Milestones — each verified on hardware before the next

| | Milestone | Proves |
|---|---|---|
| **M0** | Pico 2 W in the chassis, shipped `bluetooth.py` running, phone app drives it | the swap is real, the motors and battery work, and there is a known-good baseline to fall back to |
| **M1** | S3 advertises a fixed command on a timer with the recogniser running; report free internal RAM before and after | **the go/no-go.** BLE and ESP-SR coexist on this board, or they do not |
| **M2** | Pico 2 W observes, prints the decoded packet. Robot on blocks, wheels free | the radio path and the packet format, with nothing that can drive away |
| **M3** | Receiver drives the motors. Pull power on the S3 mid-drive; the robot must stop by itself | the dead-man — the one safety property that cannot be argued into existence |
| **M4** | Voice on the S3: six commands, variants, per-command floors, arrow on screen, records to SD. Wheels still off the ground | the recogniser half, and the first honest false-positive count |
| **M5** | On the floor in a real room. Measure latency and the false-GO rate over a session | it is a robot, not a demo |
| **M6** | The child drives it | the only test that matters |

M0 and M1 are the two unknowns; everything after them is assembly. Same shape as 02.

## Decisions

- **Name `03_voice_remote_en`**, multilingual word book slides to `04_word_book_multi`.
  The `_en` suffix carries the same meaning as in 02: MultiNet speaks English and Chinese
  only, and this is the English one.
- **The board is the remote, the robot is dumb.** Argued at the top.
- **Vocabulary is compiled in**, not on the card.
- **BLE, not ESP-NOW.** Forced by the receiver hardware.
- **The dead-man lives in the packet**, not in the receiver's configuration.

## Still open

1. **Buy a Pico 2 W?** Gates M1. If no, the plan reverts to option B and the JDY-33 BLE
   serial becomes a risk to prove at M1 instead.
2. **Continuous or momentary.** Does "GO" drive until "STOP", or drive for a fixed
   distance and stop? Momentary is far safer with a toddler; continuous is more fun.
   Default until told otherwise: momentary, TTL ~1 s, and "GO" again to keep going.
3. **What SPIN means** — 180°, 360°, or spin until stopped. Default: 360° then stop.
4. **Whether the robot's own sensors override.** It has ultrasonic and two IR obstacle
   sensors; letting them veto a GO into a wall is nearly free and fails safe. Default:
   yes, obstacle stops the robot regardless of what the board said.
5. **8 MB factory partition** — needs a full erase-and-flash round on the S3. Owed since
   02 either way.

None of these block M0.

## Sources

- Pico2Go wiki: <https://www.waveshare.com/wiki/Pico2Go>
- Pico2Go product page: <https://www.waveshare.com/pico2go-kit.htm>
- RP2350-Plus wiki: <https://www.waveshare.com/wiki/RP2350-Plus>
- RP2350B-Plus-W (the wireless variant, for contrast): <https://www.waveshare.com/rp2350b-plus-w.htm>
- JDY-33 manual: <https://manuals.plus/shenzhen/2axm8-jdy-33-dual-mode-bluetoothserial-porttransparent-transmission-module-manual>
- Demo code audited (linked from the Pico2Go wiki): <https://files.waveshare.com/wiki/PicoGo/PicoGo_Code_V2.zip>
- Schematic to verify pins against: <https://files.waveshare.com/upload/8/8b/PicoGo_Schematic_V2.pdf>
- In-repo precedent: `ESP32-C6-Touch-AMOLED-1.8/projects/16_bitchat_relay` (NimBLE advertiser),
  `.../18_govee_monitor` (NimBLE observer parsing manufacturer data)

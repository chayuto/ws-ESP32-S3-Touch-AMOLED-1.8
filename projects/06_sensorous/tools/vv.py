#!/usr/bin/env python3
"""Check what the board actually wrote against what the design says it writes.

    tools/vv.py [datadir]            # default: the data/ beside this script's project

Reads the JSON Lines the board produced (sensors, radio, power, events) and
prints a PASS/FAIL line per check. It is the verification half of a run: the
serial console proves the firmware is alive, this proves the product - the
files - is complete, well formed, and carries values in ranges a real board
produces. Every check names the record and field it failed on, because a check
that only says FAIL costs another run to interpret.

Exit status is 1 if any check failed, so it can gate a run in a script.
"""
import json
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

DATA = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "data")

fails = []
notes = []


def check(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  - ' + detail) if detail else ''}")
    if not ok:
        fails.append(name)


def load(fname):
    """JSON Lines, tolerant of a final truncated line: the board can be cut off
    mid-write by a download, and that is not a fault in the data before it."""
    p = DATA / fname
    if not p.exists():
        return None, 0
    rows, bad = [], 0
    lines = p.read_text(errors="replace").splitlines()
    for i, line in enumerate(lines):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            if i == len(lines) - 1:
                notes.append(f"{fname}: last line is truncated (a write in flight); ignored")
            else:
                bad += 1
    return rows, bad


def last_boot(rows):
    """Only the most recent run. up_ms restarts at each boot and the files are
    appended across boots, so anything computed over the whole file mixes runs -
    it reported a 0.9 s median cadence for a board recording every 10 s, and
    failed checks on faults that were fixed three boots ago. Pass --all to keep
    the lot."""
    if not rows or "--all" in sys.argv:
        return rows
    out, seen = [rows[0]], rows[0].get("up_ms")
    for r in rows[1:]:
        up = r.get("up_ms")
        # ap and ble rows carry no up_ms - they belong to the scan line before
        # them. Only a row that has one can mark the start of a new boot.
        if up is not None:
            if seen is not None and up < seen:
                out = []
            seen = up
        out.append(r)
    return out


def span_s(rows):
    ts = [r["up_ms"] for r in rows if "up_ms" in r]
    return (max(ts) - min(ts)) / 1000 if len(ts) > 1 else 0


print(f"reading {DATA}\n")

# ---- events: the boot record is the board's account of itself ---------------
events, bad = load("events.jsonl")
check(events is not None, "events.jsonl exists")
if events:
    check(bad == 0, "events.jsonl parses", f"{bad} unparseable lines")
    boots = [r for r in events if r.get("t") == "boot"]
    check(len(boots) > 0, "a boot record was written", f"{len(boots)} boots in the file")
    if boots:
        b = boots[-1]
        chip = b.get("chip", {})
        card = b.get("card", {})
        check(chip.get("freq_mhz") == 240, "CPU is at 240 MHz", f"{chip.get('freq_mhz')} MHz")
        check(chip.get("cores") == 2, "both cores reported", str(chip.get("cores")))
        check(chip.get("psram_mb", 0) >= 8, "8 MB PSRAM seen", f"{chip.get('psram_mb')} MB")
        check(b.get("imu", {}).get("present") is True, "IMU present at boot",
              f"who_am_i {b.get('imu', {}).get('who_am_i')}")
        check(card.get("present") is True, "card mounted at boot")
        check(card.get("free_mb", 0) > 0, "card free space is known at boot",
              f"{card.get('free_mb')} of {card.get('total_mb')} MB")
        check(b.get("clock", {}).get("source") in ("ntp", "rtc"), "clock has a source",
              str(b.get("clock", {}).get("source")))

# ---- sensors: the main product ---------------------------------------------
sensors, bad = load("sensors.jsonl")
sensors = last_boot(sensors) if sensors else sensors
check(sensors is not None and len(sensors) > 0, "sensors.jsonl has records",
      f"{len(sensors) if sensors else 0} records")
if sensors:
    check(bad == 0, "sensors.jsonl parses", f"{bad} unparseable lines")
    have_imu = [r for r in sensors if r.get("imu")]
    have_snd = [r for r in sensors if r.get("sound")]
    check(len(have_imu) == len(sensors), "every record carries IMU data",
          f"{len(sensors) - len(have_imu)} records with imu:null")
    check(len(have_snd) == len(sensors), "every record carries a sound level",
          f"{len(sensors) - len(have_snd)} records with sound:null")

    # Cadence: consecutive up_ms should sit near the configured interval.
    ups = sorted(r["up_ms"] for r in sensors if "up_ms" in r)
    gaps = [(b - a) / 1000 for a, b in zip(ups, ups[1:])]
    if gaps:
        worst = max(gaps)
        med = sorted(gaps)[len(gaps) // 2]
        check(worst < med * 2 + 2, "the sensor cadence is regular",
              f"median {med:.1f} s, worst {worst:.1f} s")

    if have_imu:
        i = have_imu[-1]["imu"]
        # m/s2: imu.c puts the QMI8658 driver in mps2 mode, so gravity is 9.807.
        amag = [r["imu"]["amag"] for r in have_imu if r["imu"].get("amag") is not None]
        check(all(5.0 < a < 15.0 for a in amag), "|a| stays near gravity",
              f"{min(amag):.3f}..{max(amag):.3f} m/s2 (gravity 9.807)")
        rest = min(amag)
        bias = rest - 9.80665
        if abs(bias) > 0.2:
            notes.append(f"the accelerometer reads {bias:+.3f} m/s2 ({bias/9.80665*100:+.1f}%) high at "
                         f"rest, so dyn_rms has a floor of about {abs(bias):.2f} and cannot reach 0")
        errs = sum(r["imu"].get("err", 0) for r in have_imu)
        check(errs == 0, "no IMU read errors", f"{errs} errors")
        samples = [r["imu"].get("n", 0) for r in have_imu]
        check(min(samples) > 0, "the IMU window is never empty", f"fewest {min(samples)} samples")
        check(-20 < i.get("temp_c", 0) < 90, "IMU die temperature is sane", f"{i.get('temp_c')} C")

    if have_snd:
        db = [r["sound"]["rms_dbfs"] for r in have_snd]
        clipped = sum(r["sound"].get("clipped", 0) for r in have_snd)
        check(all(-120 <= d <= 0 for d in db), "sound level is in dBFS range",
              f"{min(db):.1f}..{max(db):.1f} dBFS")
        check(all(d > -119 for d in db), "the microphone is hearing something",
              "a floor reading means the mic returned silence")
        check(clipped == 0, "no clipped samples", f"{clipped} clipped - turn the mic gain down")

    temps = [r["temp"]["board_c"] for r in sensors if r.get("temp")]
    check(all(10 < t < 70 for t in temps), "board temperature is sane and below the guard",
          f"{min(temps):.1f}..{max(temps):.1f} C")
    lv = Counter(r["temp"]["level"] for r in sensors if r.get("temp"))
    check(set(lv) <= {"ok"}, "the thermal guard never tripped", str(dict(lv)))

    pw = [r["power"] for r in sensors if r.get("power")]
    check(all(2500 < p["vbat_mv"] < 4400 for p in pw), "battery voltage is sane",
          f"{min(p['vbat_mv'] for p in pw)}..{max(p['vbat_mv'] for p in pw)} mV")

    heap = [r["heap"]["int_free"] for r in sensors if r.get("heap")]
    if len(heap) > 3:
        drift = heap[0] - heap[-1]
        check(abs(drift) < 8192, "internal heap is not draining",
              f"{heap[0]} -> {heap[-1]} B ({drift:+d} over {span_s(sensors)/60:.1f} min)")
        check(min(heap) > 12000, "internal heap keeps headroom", f"low water {min(heap)} B")

    cards = [r["card"] for r in sensors if r.get("card")]
    if cards:
        check(all(c["present"] for c in cards), "the card stayed mounted")
        check(all(c.get("free_mb", 0) > 0 for c in cards), "free space is known in every record")
        check(sum(c.get("drops", 0) for c in cards) == 0, "no log lines were dropped",
              f"{max(c.get('drops', 0) for c in cards)} drops")
        check(sum(c.get("io_err", 0) for c in cards) == 0, "no card I/O errors",
              f"{max(c.get('io_err', 0) for c in cards)} errors")

# ---- radio: the location signal --------------------------------------------
radio, bad = load("radio.jsonl")
radio = last_boot(radio) if radio else radio
check(radio is not None and len(radio) > 0, "radio.jsonl has records",
      f"{len(radio) if radio else 0} records")
if radio:
    check(bad == 0, "radio.jsonl parses", f"{bad} unparseable lines")
    kinds = Counter(r.get("t") for r in radio)
    print(f"      record kinds: {dict(kinds)}")
    check(kinds["scan"] > 0 and kinds["scan_end"] > 0, "scan cycles are bracketed",
          f"{kinds['scan']} begun, {kinds['scan_end']} ended")
    check(kinds["scan"] == kinds["scan_end"], "every scan that began also ended")
    check(kinds["ap"] > 0, "Wi-Fi APs were heard", f"{kinds['ap']} ap rows")
    check(kinds["ble"] > 0, "BLE devices were heard", f"{kinds['ble']} ble rows")

    aps = [r for r in radio if r.get("t") == "ap"]
    # Google Geolocation wants exactly these four; a missing one breaks locate.py.
    need = ("macAddress", "signalStrength", "channel", "age")
    missing = [f for f in need if any(f not in a for a in aps)]
    check(not missing, "every ap row has the Geolocation fields", f"missing {missing}")
    check(all(-100 <= a["signalStrength"] <= 0 for a in aps), "AP RSSI is in range",
          f"{min(a['signalStrength'] for a in aps)}..{max(a['signalStrength'] for a in aps)} dBm")
    check(all(1 <= a["channel"] <= 14 for a in aps), "AP channels are 2.4 GHz channels")
    check(all(len(a["macAddress"]) == 17 for a in aps), "BSSIDs are formatted as MACs")

    bles = [r for r in radio if r.get("t") == "ble"]
    if bles:
        at = Counter(b.get("addrType") for b in bles)
        print(f"      BLE address types: {dict(at)}")
        usable = at.get("public", 0) + at.get("random_static", 0)
        check(all(-127 <= b["signalStrength"] <= 0 for b in bles), "BLE RSSI is in range")
        check(all(b.get("adverts", 0) > 0 for b in bles), "every BLE row has at least one advert")
        notes.append(f"{usable} of {len(bles)} BLE rows are fixed addresses worth keeping; "
                     f"the rest are rotating and are an occupancy signal only")

    ends = [r for r in radio if r.get("t") == "scan_end"]
    if ends:
        check(not any(e.get("wifi_full") or e.get("ble_full") for e in ends),
              "no census table overflowed")
        wms = [e["wifi_ms"] for e in ends]
        bms = [e["ble_ms"] for e in ends]
        check(max(bms) < 6000, "BLE windows close on time", f"worst {max(bms)} ms")
        notes.append(f"scan cost: Wi-Fi {min(wms)}-{max(wms)} ms, BLE {min(bms)}-{max(bms)} ms")

# ---- power -----------------------------------------------------------------
power, bad = load("power.jsonl")
power = last_boot(power) if power else power
if power:
    check(bad == 0, "power.jsonl parses", f"{bad} unparseable lines")
    check(all("rails" in p for p in power), "every power record carries the rail bitmaps")
    check(all(p.get("vbat_mv", 0) > 0 for p in power), "battery is reported in every record")

# ---- the log file the board mirrors ----------------------------------------
logp = DATA / "sensorous.log"
if logp.exists():
    txt = logp.read_text(errors="replace")
    # Same reason as last_boot(): the log is appended across boots and the earlier
    # ones carry faults that are fixed. Cut to the last start banner.
    if "--all" not in sys.argv and "===== boot:" in txt:
        txt = txt[txt.rindex("===== boot:"):]
    errs = [ln for ln in txt.splitlines() if ln.startswith("E (")]
    warns = [ln for ln in txt.splitlines() if ln.startswith("W (")]
    # These two are documented as expected on this board; anything else is not.
    expected = ("pull-up resistance",       # the board has hardware pull-ups
                "3Ah command",               # the BSP supplies its own init list
                "Long filenames",            # the BSP tests a Kconfig name that no longer exists
                "authmode threshold changes",  # esp_wifi on a WPA2 password
                "mode stationary ->", "mode mobile ->", "-> stationary", "-> mobile",
                "maintenance up at", "maintenance down")  # the app states its own mode changes at W
    unexpected = [w for w in warns if not any(e in w for e in expected)]
    # Printed whether or not they fail a check: a warning this list learns to
    # expect is still something a reader of the run should see.
    for w in warns[:8]:
        print(f"      W: {w.split(') ', 1)[-1][:100]}")
    if len(warns) > 8:
        print(f"      ... and {len(warns) - 8} more W lines")
    check(not errs, "no E lines in the mirrored log", f"{len(errs)} errors, first: "
          f"{errs[0][:90] if errs else ''}")
    check(not unexpected, "no unexpected W lines", f"{len(unexpected)} warnings, first: "
          f"{unexpected[0][:90] if unexpected else ''}")

if sensors:
    print(f"\n{len(sensors)} sensor records over {span_s(sensors)/60:.1f} min"
          f" ({len(sensors)/max(span_s(sensors)/3600, 1e-9):.0f}/hour)")
for n in notes:
    print(f"note: {n}")
print(f"\n{len(fails)} failed" if fails else "\nall checks passed")
sys.exit(1 if fails else 0)

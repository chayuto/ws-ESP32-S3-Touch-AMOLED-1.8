#!/usr/bin/env python3
"""
Turn radio.jsonl from the card into location fixes.

The board cannot know where it is: no GNSS, no cell modem. What it can do is
write down every Wi-Fi BSSID it hears with a signal strength, and BSSIDs belong
to boxes that do not move. Google's Geolocation API turns a set of them into a
lat/long, and this script is the join between the two.

    # what did it hear, and where would that put it?
    ./locate.py radio.jsonl                       # summarise, no network
    ./locate.py radio.jsonl --key $GOOGLE_API_KEY # resolve every scan
    ./locate.py radio.jsonl --key $K --geojson out.geojson

The `ap` records already carry Google's own field names - macAddress,
signalStrength, channel, age - so a request body is a filter and a rename away.
Nothing is uploaded unless --key is given.

BLE is summarised but never sent: Google's `bluetoothBeacons` only matches
beacons registered with them, and most of what the board hears is a phone using
a resolvable private address that rotates every quarter hour. Those rows are
recorded because they say how crowded a place is, not where it is.
"""

import argparse
import collections
import json
import sys
import urllib.request

GEOLOCATE = "https://www.googleapis.com/geolocation/v1/geolocate?key="

# Google wants at least two APs before it will attempt a fix; one is not a
# position, it is a guess at the shape of a database.
MIN_APS = 2


def read_scans(path):
    """Group the JSON Lines back into scan cycles, keyed by `seq`."""
    scans = collections.OrderedDict()
    bad = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                # A card pulled mid-write truncates the final line. Everything
                # before it is still good, which is the point of JSON Lines.
                bad += 1
                continue
            t = rec.get("t")
            if t not in ("scan", "ap", "ble", "scan_end"):
                continue
            seq = rec.get("seq")
            s = scans.setdefault(seq, {"seq": seq, "aps": [], "ble": [], "time": None, "mode": None})
            if t == "scan":
                s["time"] = rec.get("time")
                s["mode"] = rec.get("mode")
            elif t == "ap":
                s["aps"].append(rec)
            elif t == "ble":
                s["ble"].append(rec)
    if bad:
        print(f"note: skipped {bad} unparseable line(s) - a truncated tail is normal", file=sys.stderr)
    return list(scans.values())


def request_body(aps):
    """Exactly what the Geolocation API asks for, and nothing else."""
    return {
        "considerIp": False,
        "wifiAccessPoints": [
            {
                "macAddress": a["macAddress"],
                "signalStrength": a["signalStrength"],
                "channel": a.get("channel", 0),
                "age": a.get("age", 0),
            }
            for a in aps
        ],
    }


def geolocate(key, aps):
    body = json.dumps(request_body(aps)).encode()
    req = urllib.request.Request(
        GEOLOCATE + key, data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.load(resp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="radio.jsonl from the card")
    ap.add_argument("--key", help="Google API key. Without it, nothing leaves this machine.")
    ap.add_argument("--geojson", help="write the resolved fixes here as GeoJSON")
    ap.add_argument("--limit", type=int, default=0, help="resolve at most this many scans")
    ap.add_argument("--every", type=int, default=1, help="resolve every Nth scan (a stationary run repeats)")
    args = ap.parse_args()

    scans = read_scans(args.file)
    if not scans:
        print("no scan records in that file", file=sys.stderr)
        return 1

    total_aps = sum(len(s["aps"]) for s in scans)
    total_ble = sum(len(s["ble"]) for s in scans)
    unique_bssids = {a["macAddress"] for s in scans for a in s["aps"]}
    print(f"{len(scans)} scan cycles: {total_aps} AP sightings ({len(unique_bssids)} distinct BSSIDs), "
          f"{total_ble} BLE sightings")

    usable = [s for s in scans if len(s["aps"]) >= MIN_APS]
    print(f"{len(usable)} of them have the {MIN_APS}+ access points a fix needs")

    if not args.key:
        print("\nno --key, so nothing was sent. A request body for the first usable scan:")
        if usable:
            print(json.dumps(request_body(usable[0]["aps"][:10]), indent=2))
        return 0

    chosen = usable[:: args.every]
    if args.limit:
        chosen = chosen[: args.limit]

    features = []
    for s in chosen:
        try:
            r = geolocate(args.key, s["aps"])
        except Exception as e:  # noqa: BLE001 - one bad scan must not stop the run
            print(f"scan {s['seq']}: {e}", file=sys.stderr)
            continue
        loc, acc = r["location"], r.get("accuracy")
        print(f"scan {s['seq']} {s['time']}: {loc['lat']:.6f},{loc['lng']:.6f}  +/-{acc:.0f} m  "
              f"({len(s['aps'])} APs)")
        features.append({
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [loc["lng"], loc["lat"]]},
            "properties": {
                "seq": s["seq"], "time": s["time"], "mode": s["mode"],
                "accuracy_m": acc, "aps": len(s["aps"]), "ble": len(s["ble"]),
            },
        })

    if args.geojson and features:
        with open(args.geojson, "w", encoding="utf-8") as f:
            json.dump({"type": "FeatureCollection", "features": features}, f, indent=1)
        print(f"\nwrote {len(features)} fixes to {args.geojson}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

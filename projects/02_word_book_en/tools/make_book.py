#!/usr/bin/env python3
"""Build a word-book content folder for the SD card.

    python3 tools/make_book.py ~/Pictures/wordbook  /Volumes/SDCARD/book
    python3 tools/make_book.py --demo               /Volumes/SDCARD/book

Input: a folder of photos named after the word — dog.jpg, cat.png, mama.jpeg —
and optionally a recording per word saying it — dog.wav, dog.m4a, dog.aiff.
The filename stem *is* the word; case does not matter.

Output, in the layout the firmware reads:

    book/
      words.json      one entry per word: text, and which files exist
      dog.rgb565      368x448, 16-bit RGB565 little-endian, 329,728 bytes, no header
      dog.wav         16 kHz, mono, 16-bit PCM
      ...

Photos are centre-cropped to 368x448 (portrait). Recordings are resampled with
macOS afconvert if present, otherwise must already be 16 kHz mono 16-bit WAV.

--demo generates three placeholder cards (DOG, CAT, BALL) so the SD path can be
verified before any real photos exist.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 368, 448
PHOTO_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
AUDIO_EXT = {".wav", ".m4a", ".aiff", ".aif", ".mp3", ".caf"}


def to_rgb565(img: Image.Image) -> bytes:
    """Centre-crop to 368x448 and pack as little-endian RGB565."""
    src_ratio = img.width / img.height
    dst_ratio = W / H
    if src_ratio > dst_ratio:  # too wide: crop sides
        new_w = int(img.height * dst_ratio)
        left = (img.width - new_w) // 2
        img = img.crop((left, 0, left + new_w, img.height))
    else:  # too tall: crop top/bottom
        new_h = int(img.width / dst_ratio)
        top = (img.height - new_h) // 2
        img = img.crop((0, top, img.width, top + new_h))
    img = img.convert("RGB").resize((W, H), Image.LANCZOS)

    out = bytearray(W * H * 2)
    i = 0
    for r, g, b in img.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i] = v & 0xFF
        out[i + 1] = v >> 8
        i += 2
    return bytes(out)


def convert_audio(src: Path, dst: Path) -> bool:
    if src.suffix.lower() == ".wav":
        with wave.open(str(src)) as w:
            if w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2:
                shutil.copyfile(src, dst)
                return True
    if shutil.which("afconvert"):
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", str(src), str(dst)],
                       check=True, capture_output=True)
        return True
    print(f"  ! {src.name}: not 16 kHz mono 16-bit WAV and afconvert is not available; skipped", file=sys.stderr)
    return False


def demo_card(word: str, color: tuple) -> Image.Image:
    img = Image.new("RGB", (W, H), color)
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 96)
    except OSError:
        font = ImageFont.load_default()
    bbox = d.textbbox((0, 0), word, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((W - tw) / 2 - bbox[0], (H - th) / 2 - bbox[1]), word, fill="white", font=font)
    d.text((16, H - 40), "demo card", fill=(255, 255, 255, 128), font=ImageFont.load_default())
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", help="folder of photos (and optional recordings)")
    ap.add_argument("dst", help="output folder, e.g. /Volumes/SDCARD/book")
    ap.add_argument("--demo", action="store_true", help="generate placeholder cards instead of reading src")
    args = ap.parse_args()

    dst = Path(args.dst)
    dst.mkdir(parents=True, exist_ok=True)
    words = {}

    if args.demo:
        for word, color in [("DOG", (180, 80, 40)), ("CAT", (60, 120, 180)), ("BALL", (200, 40, 90))]:
            (dst / f"{word.lower()}.rgb565").write_bytes(to_rgb565(demo_card(word, color)))
            words[word] = {"photo": f"{word.lower()}.rgb565"}
            print(f"  {word:8s} demo card")
    else:
        if not args.src:
            ap.error("src is required unless --demo")
        src = Path(args.src)
        for p in sorted(src.iterdir()):
            stem, ext = p.stem.strip().upper(), p.suffix.lower()
            if not stem or stem.startswith("."):
                continue
            entry = words.setdefault(stem, {})
            if ext in PHOTO_EXT:
                (dst / f"{stem.lower()}.rgb565").write_bytes(to_rgb565(Image.open(p)))
                entry["photo"] = f"{stem.lower()}.rgb565"
                print(f"  {stem:8s} photo  <- {p.name}")
            elif ext in AUDIO_EXT:
                if convert_audio(p, dst / f"{stem.lower()}.wav"):
                    entry["prompt"] = f"{stem.lower()}.wav"
                    print(f"  {stem:8s} prompt <- {p.name}")

    manifest = {
        "format": 1,
        "photo": {"width": W, "height": H, "pixel": "rgb565le"},
        "audio": {"rate": 16000, "channels": 1, "bits": 16},
        "words": [{"text": w, **files} for w, files in sorted(words.items())],
    }
    (dst / "words.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {dst / 'words.json'}: {len(words)} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

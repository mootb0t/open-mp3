"""Converts albums into the layout the SD_Player firmware reads from the SD card.

Usage:  python3 prepare_sd.py albums.json --out <folder> [--adpcm]
        then copy <folder>/music onto the SD card (FAT32), so the card has /music/...
        (with a card reader, or over the ESP32's USB cable with upload_music.py)

albums.json lists each album: source folder, title, artist, year, cover image, and the
tracks in order as [file, title] pairs (copy albums.example.json to albums.json and edit it).

On the card, each album becomes a folder:
  /music/2004 The College Dropout/
      album.txt     line 1 title, line 2 artist, line 3 year, then "01.wav|Track title" per track
      cover136.rgb  cover art, 136x136, raw RGB565 big-endian (for the album browser)
      cover112.rgb  cover art, 112x112, raw RGB565 big-endian (for the now-playing screen)
      01.wav ...    stereo 44.1 kHz: 16-bit PCM, or with --adpcm, IMA ADPCM in 2048-byte blocks (4x smaller)
Albums are listed on the player in folder-name order, so the year prefix sorts them by date.
Needs ffmpeg.
"""
import argparse
import json
import os
import re
import subprocess
import sys


def run(cmd):
    subprocess.run(cmd, check=True, capture_output=True)


def cover(src, size, dst):
    run(["ffmpeg", "-y", "-v", "error", "-i", src, "-vf", f"scale={size}:{size}:flags=lanczos",
         "-f", "rawvideo", "-pix_fmt", "rgb565be", dst])
    if os.path.getsize(dst) != size * size * 2:
        sys.exit(f"cover conversion went wrong: {dst}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("--out", required=True)
    ap.add_argument("--adpcm", action="store_true",
                    help="compress 4:1 (IMA ADPCM) for a quicker copy over the ESP32's USB cable")
    args = ap.parse_args()

    albums = json.load(open(args.config))
    codec = ["-c:a", "adpcm_ima_wav", "-block_size", "2048"] if args.adpcm else ["-c:a", "pcm_s16le"]
    for a in albums:
        src = os.path.expanduser(a["folder"])
        for f, _ in a["tracks"]:
            if not os.path.isfile(os.path.join(src, f)):
                sys.exit(f"Missing: {os.path.join(src, f)}")

    for a in albums:
        src = os.path.expanduser(a["folder"])
        name = re.sub(r'[\\/:*?"<>|]', "", f"{a['year']} {a['title']}")
        dst = os.path.join(args.out, "music", name)
        os.makedirs(dst, exist_ok=True)
        print(f"{name}: {len(a['tracks'])} tracks", flush=True)

        cover(os.path.join(src, a["cover"]), 136, os.path.join(dst, "cover136.rgb"))
        cover(os.path.join(src, a["cover"]), 112, os.path.join(dst, "cover112.rgb"))

        lines = [a["title"], a["artist"], str(a["year"])]
        for i, (f, title) in enumerate(a["tracks"], 1):
            wav = f"{i:02d}.wav"
            out = os.path.join(dst, wav)
            if not os.path.exists(out):
                print(f"  {wav}  {title}", flush=True)
                run(["ffmpeg", "-y", "-v", "error", "-i", os.path.join(src, f), "-vn", "-map_metadata", "-1",
                     "-ar", "44100", "-ac", "2", *codec, "-fflags", "+bitexact", out + ".tmp.wav"])
                os.replace(out + ".tmp.wav", out)
            lines.append(f"{wav}|{title}")
        with open(os.path.join(dst, "album.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")

    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(os.path.join(args.out, "music")) for f in fs)
    print(f"Done: {total / 1e9:.2f} GB in {os.path.join(args.out, 'music')}")


if __name__ == "__main__":
    main()

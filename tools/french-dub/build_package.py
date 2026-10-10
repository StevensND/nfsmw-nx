"""Build the French dub package in one go: text, movies and speech, zipped for the SD card.

The zip holds nfsmw-nx/game_root/NFS/... and nfsmw-nx/game_root/Movies/..., to extract into
sdmc:/switch/ over an existing install (like the installer page's nfsmw-nx.zip). It contains game
data made from your own discs: do not share it.

Usage: python build_package.py [--x360 ISO] [--xbox ISO] [--out ZIP] [--ffmpeg FFMPEG] [--vgmstream CLI]
Missing disc images and the output file are asked with file dialogs. Takes about an hour.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def ask_open(title):
    import tkinter
    from tkinter import filedialog
    root = tkinter.Tk()
    root.withdraw()
    chosen = filedialog.askopenfilename(title=title, filetypes=[("Disc image", "*.iso"), ("All files", "*.*")])
    root.destroy()
    if not chosen:
        raise SystemExit("cancelled")
    return chosen


def ask_save(title, initial):
    import tkinter
    from tkinter import filedialog
    root = tkinter.Tk()
    root.withdraw()
    chosen = filedialog.asksaveasfilename(title=title, initialfile=initial, defaultextension=".zip",
                                          filetypes=[("Zip", "*.zip")])
    root.destroy()
    if not chosen:
        raise SystemExit("cancelled")
    return chosen


def run(step, *args):
    print(f"\n=== {step}", flush=True)
    t = time.time()
    subprocess.run([sys.executable, "-u", *args], cwd=HERE, check=True)
    print(f"=== {step}: {(time.time() - t) / 60:.0f} min", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--x360", help="Xbox 360 PAL (English) disc image")
    p.add_argument("--xbox", help="original Xbox French disc image")
    p.add_argument("--out", help="zip to write")
    p.add_argument("--ffmpeg", default=shutil.which("ffmpeg"), help="ffmpeg (with ffprobe next to it)")
    p.add_argument("--vgmstream", default=shutil.which("vgmstream-cli")
                   or os.path.join(HERE, "vgmstream", "vgmstream-cli.exe"), help="vgmstream-cli")
    a = p.parse_args()
    x360 = os.path.abspath(a.x360 or ask_open("Xbox 360 image, PAL (English) edition"))
    xbox = os.path.abspath(a.xbox or ask_open("Original Xbox image, French edition"))
    out = os.path.abspath(a.out or ask_save("Zip to write", "nfsmw-nx-vf.zip"))
    for tool, name in ((a.ffmpeg, "ffmpeg"), (a.vgmstream, "vgmstream-cli")):
        if not tool or not os.path.exists(tool):
            raise SystemExit(f"{name} not found: give its path with --{name.split('-')[0]}")

    start = time.time()
    with tempfile.TemporaryDirectory(dir=os.path.dirname(out)) as work:
        game_root = os.path.join(work, "game_root")
        run("1/3 text", "patch_text.py", x360, xbox, game_root)
        run("2/3 movies", "patch_movies.py", x360, xbox, os.path.join(game_root, "Movies"), a.ffmpeg)
        run("3/3 speech (about 45 min)", "patch_speech.py", x360, xbox, game_root, a.ffmpeg, a.vgmstream)
        print("\n=== zip", flush=True)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
            for folder in ("NFS", "Movies"):
                for name in sorted(os.listdir(os.path.join(game_root, folder))):
                    if name.lower().endswith((".bin", ".wmv")):
                        z.write(os.path.join(game_root, folder, name), f"nfsmw-nx/game_root/{folder}/{name}")
        shutil.copy(os.path.join(game_root, "Movies", "movies_report.txt"),
                    os.path.splitext(out)[0] + "_movies_report.txt")
    print(f"\nDone in {(time.time() - start) / 60:.0f} min: {out}")
    print("Extract it into sdmc:/switch/ (it replaces 3 files in NFS and 32 movies; back them up first).")


if __name__ == "__main__":
    main()

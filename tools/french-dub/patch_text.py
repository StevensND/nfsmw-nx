"""Step 1: French text.

The Xbox 360 PAL (English) disc already contains the French text, but its executable loads the English
files (and XGetLanguage in the SDK always returns English, so user_language does not help):
- LANGUAGES\\ENGLISH.BIN -> LANGUAGES\\FRENCH.BIN: menus and game text;
- LOCH 21d83a59 -> LOCH a8a7b105: UTF-16 system messages ("Loading %1. Please don't turn off your
  console." -> "Chargement de %1 en cours. Ne pas eteindre la console."). Their names are unknown,
  the hashes come from ZDIR.BIN.

Two changes, without touching the game:
1. NFS/ZDIR.BIN: the entry of the English file points to the data of the French one (the name hash
   stays, ZZDATA number, sector, global sector, size and checksum are copied).
2. GLOBAL\\GLOBALMEMORYFILE.BIN, a pack preloaded at startup, holds a copy of the English files that
   the game uses before looking at ZDIR.BIN (with change 1 alone, the title screen stayed in
   English). Their entries in the pack's table are renamed to the hash of a name that does not
   exist, so the game reads the files through ZDIR.BIN. The pack lives in a ZZDATA file: a copy of
   that ZZDATA is written with those bytes changed.

Pack table: 8 zero bytes, magic 0x53219999, entry count, then 20 bytes per entry
(name hash, offset, size in memory, stored size, flags).

Usage: python patch_text.py <Xbox 360 image> <Xbox image> <output game_root>
"""
import os
import struct
import sys

from zzdata import SECTOR, bstringhash, open_discs, path

PACK = path("GLOBAL", "GLOBALMEMORYFILE.BIN")
PACK_MAGIC = 0x53219999
REPLACEMENTS = [  # (English hash, French hash, description)
    (bstringhash(path("LANGUAGES", "ENGLISH.BIN")), bstringhash(path("LANGUAGES", "FRENCH.BIN")),
     "LANGUAGES\\ENGLISH.BIN -> FRENCH.BIN"),
    (0x21D83A59, 0xA8A7B105, "LOCH system messages, English -> French"),
]


def hidden(h):
    """Hash of a name that does not exist, to hide an entry of the pack."""
    return bstringhash(path("FRENCH_DUB", f"HIDDEN_{h:08X}"))


def zdir_entry(zdir, h):
    for i in range(0, len(zdir) - 23, 24):
        if struct.unpack_from("<I", zdir, i)[0] == h:
            return i
    raise KeyError(f"{h:08x}")


def patch_zdir(disc, output):
    zdir = bytearray(disc.zdir)
    for en, fr, what in REPLACEMENTS:
        e = zdir_entry(zdir, en)
        f = zdir_entry(zdir, fr)
        zdir[e + 4:e + 24] = zdir[f + 4:f + 24]
        print(f"ZDIR.BIN: {what}")
    with open(os.path.join(output, "NFS", "ZDIR.BIN"), "wb") as w:
        w.write(zdir)


def patch_pack(disc, output):
    pack = disc.read(PACK)
    magic, count = struct.unpack_from("<II", pack, 8)
    if magic != PACK_MAGIC:
        raise ValueError(f"{PACK}: unexpected magic {magic:#x}")
    positions = {struct.unpack_from("<I", pack, 16 + 20 * i)[0]: 16 + 20 * i for i in range(count)}
    number, sector, _ = disc.entries[bstringhash(PACK)]
    name = f"ZZDATA{number}.BIN"
    size, start = disc.files[f"NFS/{name}"]
    with open(disc.iso, "rb") as f, open(os.path.join(output, "NFS", name), "wb") as w:
        f.seek(start)
        left = size
        while left:
            block = f.read(min(left, 16 << 20))
            w.write(block)
            left -= len(block)
        for en, _, what in REPLACEMENTS:
            if en not in positions:
                print(f"{PACK}: no copy for {what}")
                continue
            w.seek(sector * SECTOR + positions[en])
            w.write(struct.pack("<I", hidden(en)))
            print(f"{name}: copy hidden in {PACK} ({what})")


def main():
    iso360, iso_xbox, output = sys.argv[1:4]
    x360, _ = open_discs(iso360, iso_xbox)
    os.makedirs(os.path.join(output, "NFS"), exist_ok=True)
    patch_zdir(x360, output)
    patch_pack(x360, output)


if __name__ == "__main__":
    main()

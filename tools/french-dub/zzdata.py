"""Read-only access to the files packed in NFS/ZZDATA*.BIN through the NFS/ZDIR.BIN index.

ZDIR.BIN entry (24 bytes, little endian):
  name hash (bStringHash of the upper-case path), ZZDATA number, sector (2048 bytes),
  global sector (sum of the sectors of the previous ZZDATA files + sector), size, checksum
The checksum is not a standard CRC32 and does not seem to be checked by the game.
"""
import hashlib
import struct

from xdvdfs import list_files

ENTRY = 24
SECTOR = 2048
BS = chr(92)

# default.xex of the "PAL (English)" edition (same value as in the nfsmw-nx installer manifest)
PAL_ENGLISH_XEX = "e2ea2ff11bf84cb30430d824d7bfe699a3acf3e7400d21b9a7c938e3d7a10474"


def bstringhash(name):
    h = 0xFFFFFFFF
    for c in name.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def path(*parts):
    """Game path with backslashes, e.g. path("LANGUAGES", "FRENCH.BIN")."""
    return BS.join(parts)


class Disc:
    def __init__(self, iso):
        self.iso = iso
        self.files = list_files(iso)
        size, off = self.files["NFS/ZDIR.BIN"]
        with open(iso, "rb") as f:
            f.seek(off)
            self.zdir = f.read(size)
        self.entries = {}
        for i in range(0, len(self.zdir) - ENTRY + 1, ENTRY):
            h, number, sector, _, length, _ = struct.unpack_from("<6I", self.zdir, i)
            self.entries[h] = (number, sector, length)

    def has(self, name):
        return bstringhash(name) in self.entries

    def read(self, name):
        number, sector, length = self.entries[bstringhash(name)]
        _, base = self.files[f"NFS/ZZDATA{number}.BIN"]
        with open(self.iso, "rb") as f:
            f.seek(base + sector * SECTOR)
            return f.read(length)

    def read_file(self, file_path):
        """A file of the disc itself (outside ZZDATA), e.g. 'Movies/psa_english_pal.wmv'."""
        length, off = self.files[file_path.upper()]
        with open(self.iso, "rb") as f:
            f.seek(off)
            return f.read(length)

    def global_sector_base(self, number):
        """Sum of the sectors of ZZDATA0 .. ZZDATA(number - 1)."""
        return sum(self.files[f"NFS/ZZDATA{i}.BIN"][0] // SECTOR for i in range(number))


def open_discs(iso360, iso_xbox):
    """Open both images and check they are the editions this tool was tested with."""
    x360 = Disc(iso360)
    if hashlib.sha256(x360.read_file("default.xex")).hexdigest() != PAL_ENGLISH_XEX:
        raise SystemExit("the Xbox 360 image is not the PAL (English) edition this tool supports")
    xbox = Disc(iso_xbox)
    if not xbox.has(path("SOUND", "SPEECH", "COPSPEECH_FR.BIG")):
        raise SystemExit("the Xbox image is not the French edition (no COPSPEECH_FR.BIG)")
    return x360, xbox

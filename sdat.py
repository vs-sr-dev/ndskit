"""NitroSDK sound archives (SDAT): sequences, banks, wave archives, streams.

    python -m ndskit.sdat SOUND.sdat --info          # blocks and counts per kind
    python -m ndskit.sdat SOUND.sdat --list          # every record, with its name and file
    python -m ndskit.sdat SOUND.sdat --extract DIR   # the files, named by their symbols

The SDK's sound library (SND on the ARM9, its driver on the ARM7) plays
what an SDAT holds: SSEQ sequences (MIDI-like event streams for the
ARM7's sequencer), SSAR sequence archives (the effects), SBNK banks
(instruments: which wave of which archive, envelopes, key splits), SWAR
wave archives (SWAV samples: PCM8, PCM16 or IMA-ADPCM), and STRM streams
(long PCM/ADPCM tracks read from the cartridge as they play). Players set
how many sequences of a kind play at once on which channels; groups name
sets of records loaded together.

Layout: a header with four blocks. INFO has eight lists of records
(SEQ, SEQARC, BANK, WAVEARC, PLAYER, GROUP, PLAYER2, STRM), each record
giving the file id of its data in FAT; SYMB, when present, names them
(for SEQARC, also the sequences inside each archive); FAT gives each file
id its absolute offset and size; FILE holds the data.
"""
import argparse
import os
import struct

KINDS = ["SEQ", "SEQARC", "BANK", "WAVEARC", "PLAYER", "GROUP", "PLAYER2", "STRM"]
EXT = {"SEQ": "sseq", "SEQARC": "ssar", "BANK": "sbnk", "WAVEARC": "swar", "STRM": "strm"}


class Record:
    __slots__ = ("kind", "index", "name", "file_id", "fields")

    def __init__(self, kind, index, name, file_id, fields):
        self.kind, self.index, self.name, self.file_id, self.fields = kind, index, name, file_id, fields


class Sdat:
    def __init__(self, data):
        if data[:4] != b"SDAT":
            raise ValueError("not an SDAT")
        self.data = data
        nblocks = struct.unpack_from("<H", data, 0x0E)[0]
        blocks = struct.unpack_from("<8I", data, 0x10)
        if nblocks == 3:        # no SYMB: INFO, FAT, FILE follow at 0x10
            blocks = (0, 0) + blocks[:6]
        self.symb_off, self.symb_size, self.info_off, _, self.fat_off, _, self.file_off, _ = blocks
        n = struct.unpack_from("<I", data, self.fat_off + 8)[0]
        self.fat = [struct.unpack_from("<II", data, self.fat_off + 12 + 16 * i) for i in range(n)]
        self.names = {k: {} for k in KINDS}
        self.seqarc_names = {}
        if self.symb_off:
            self._symbols()
        self.records = {k: [] for k in KINDS}
        self._info()

    def _cstr(self, off):
        e = self.data.index(b"\0", off)
        return self.data[off:e].decode("latin1")

    def _symbols(self):
        d, base = self.data, self.symb_off
        offs = struct.unpack_from("<8I", d, base + 8)
        for k, o in zip(KINDS, offs):
            if not o:
                continue
            cnt = struct.unpack_from("<I", d, base + o)[0]
            for i in range(cnt):
                if k == "SEQARC":
                    no, sub = struct.unpack_from("<II", d, base + o + 4 + 8 * i)
                    if no:
                        self.names[k][i] = self._cstr(base + no)
                    if sub:
                        sc = struct.unpack_from("<I", d, base + sub)[0]
                        self.seqarc_names[i] = [
                            self._cstr(base + so) if so else None
                            for so in struct.unpack_from("<%dI" % sc, d, base + sub + 4)]
                else:
                    no = struct.unpack_from("<I", d, base + o + 4 + 4 * i)[0]
                    if no:
                        self.names[k][i] = self._cstr(base + no)

    def _info(self):
        d, base = self.data, self.info_off
        offs = struct.unpack_from("<8I", d, base + 8)
        for k, o in zip(KINDS, offs):
            if not o:
                continue
            cnt = struct.unpack_from("<I", d, base + o)[0]
            for i in range(cnt):
                ro = struct.unpack_from("<I", d, base + o + 4 + 4 * i)[0]
                if not ro:
                    continue
                p = base + ro
                fid, fields = None, {}
                if k == "SEQ":
                    fid, _, bank, vol, cpr, ppr, ply = struct.unpack_from("<HHHBBBB", d, p)
                    fields = dict(bank=bank, volume=vol, channel_priority=cpr, player_priority=ppr, player=ply)
                elif k in ("SEQARC", "WAVEARC"):
                    fid = struct.unpack_from("<H", d, p)[0]
                elif k == "BANK":
                    fid, _, *wa = struct.unpack_from("<HH4H", d, p)
                    fields = dict(wavearcs=[w for w in wa if w != 0xFFFF])
                elif k == "PLAYER":
                    smax, _, mask, heap = struct.unpack_from("<BBHI", d, p)
                    fields = dict(max_sequences=smax, channels="%04X" % mask, heap=heap)
                elif k == "GROUP":
                    n = struct.unpack_from("<I", d, p)[0]
                    fields = dict(entries=[(KINDS[t & 7] if t < 8 else t, lf, idx) for t, lf, _, idx in
                                           (struct.unpack_from("<BBHI", d, p + 4 + 8 * j) for j in range(n))])
                elif k == "PLAYER2":
                    n = d[p]
                    fields = dict(channels=list(d[p + 1:p + 1 + n]))
                elif k == "STRM":
                    fid, _, vol, pri, ply = struct.unpack_from("<HHBBB", d, p)
                    fields = dict(volume=vol, priority=pri, player=ply)
                self.records[k].append(Record(k, i, self.names[k].get(i), fid, fields))

    def file(self, fid):
        off, size = self.fat[fid]
        return self.data[off:off + size]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sdat")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", metavar="DIR")
    a = ap.parse_args(argv)
    with open(a.sdat, "rb") as f:
        s = Sdat(f.read())
    if a.info or not (a.list or a.extract):
        print("%d files, %s symbols" % (len(s.fat), "with" if s.symb_off else "no"))
        for k in KINDS:
            r = s.records[k]
            if r:
                size = sum(s.fat[x.file_id][1] for x in r if x.file_id is not None)
                print("  %-8s %4d%s" % (k, len(r), "   %8d bytes" % size if size else ""))
    if a.list:
        for k in KINDS:
            for r in s.records[k]:
                extra = ""
                if k == "SEQARC" and r.index in s.seqarc_names:
                    extra = " (%d sequences)" % len(s.seqarc_names[r.index])
                f = "file %4d %8d" % (r.file_id, s.fat[r.file_id][1]) if r.file_id is not None else " " * 18
                print("%-8s %4d  %-28s %s  %s%s" % (k, r.index, r.name or "-", f,
                                                   " ".join("%s=%s" % kv for kv in r.fields.items()), extra))
    if a.extract:
        n = 0
        for k, ext in EXT.items():
            os.makedirs(os.path.join(a.extract, ext), exist_ok=True)
            for r in s.records[k]:
                if r.file_id is None:
                    continue
                name = r.name or "%s_%03d" % (k.lower(), r.index)
                with open(os.path.join(a.extract, ext, "%s.%s" % (name, ext)), "wb") as f:
                    f.write(s.file(r.file_id))
                n += 1
        print("%d files -> %s" % (n, a.extract))


if __name__ == "__main__":
    main()

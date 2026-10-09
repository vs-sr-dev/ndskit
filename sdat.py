"""NitroSDK sound archives (SDAT): sequences, banks, wave archives, streams.

    python -m ndskit.sdat SOUND.sdat --info          # blocks and counts per kind
    python -m ndskit.sdat SOUND.sdat --list          # every record, with its name and file
    python -m ndskit.sdat SOUND.sdat --waves         # wave formats, rates, loops; PCM, PSG, noise instruments
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


WAVE_FORMATS = ["PCM8", "PCM16", "ADPCM"]
INSTRUMENTS = {0: "empty", 1: "PCM", 2: "PSG", 3: "noise", 4: "direct PCM", 5: "null",
               16: "drum set", 17: "key split"}


def swar_waves(data):
    """A SWAR's waves: (format, loops, rate, timer, loop start, length after it), the last two in bytes."""
    if data[:4] != b"SWAR":
        raise ValueError("not a SWAR")
    n = struct.unpack_from("<I", data, 0x38)[0]
    out = []
    for i in range(n):
        o = struct.unpack_from("<I", data, 0x3C + 4 * i)[0]
        fmt, loop, rate, timer, lstart, llen = struct.unpack_from("<BBHHHI", data, o)
        out.append((WAVE_FORMATS[fmt] if fmt < 3 else fmt, bool(loop), rate, timer, lstart * 4, llen * 4))
    return out


def sbnk_instruments(data):
    """A SBNK's programs: (program, type, [(type, wave, wave archive), ...]) for each one not empty.

    A drum set or a key split lists its regions' instruments; a PSG's wave is its duty cycle."""
    if data[:4] != b"SBNK":
        raise ValueError("not a SBNK")
    n = struct.unpack_from("<I", data, 0x38)[0]
    out = []
    for i in range(n):
        t, o = struct.unpack_from("<BH", data, 0x3C + 4 * i)
        if t == 0:
            continue
        if t < 16:
            subs = [(t,) + struct.unpack_from("<HH", data, o)]
        elif t == 16:
            lo, hi = data[o], data[o + 1]
            subs = [struct.unpack_from("<HHH", data, o + 2 + 12 * j) for j in range(hi - lo + 1)]
        else:
            regions = sum(1 for b in data[o:o + 8] if b)
            subs = [struct.unpack_from("<HHH", data, o + 8 + 12 * j) for j in range(regions)]
        out.append((i, t, subs))
    return out


def waves_report(s, out=print):
    """What the waves are made of and which instruments play them: formats, rates, loops; PSG and noise."""
    from collections import Counter
    total, rates = Counter(), Counter()
    for r in s.records["WAVEARC"]:
        if r.file_id is None:
            continue
        w = swar_waves(s.file(r.file_id))
        c = Counter(f for f, *_ in w)
        total.update(c)
        rates.update(rate for _, _, rate, *_ in w)
        loops = sum(1 for _, lp, *_ in w if lp)
        out("WAVEARC %4d  %-28s %4d waves  %s  %d looped" % (
            r.index, r.name or "-", len(w), " ".join("%s:%d" % kv for kv in sorted(c.items())), loops))
    out("waves: %s" % " ".join("%s:%d" % kv for kv in sorted(total.items())))
    out("rates: %s" % " ".join("%d:%d" % kv for kv in rates.most_common(8)))
    kinds, used = Counter(), Counter()
    for r in s.records["BANK"]:
        if r.file_id is None:
            continue
        for _, t, subs in sbnk_instruments(s.file(r.file_id)):
            kinds[INSTRUMENTS.get(t, t)] += 1
            used.update(INSTRUMENTS.get(st, st) for st, *_ in subs)
    out("programs: %s" % " ".join("%s:%d" % kv for kv in kinds.most_common()))
    out("instruments in them: %s" % " ".join("%s:%d" % kv for kv in used.most_common()))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sdat")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--waves", action="store_true", help="wave formats, rates, loops; instrument kinds")
    ap.add_argument("--extract", metavar="DIR")
    a = ap.parse_args(argv)
    with open(a.sdat, "rb") as f:
        s = Sdat(f.read())
    if a.waves:
        waves_report(s)
    if a.info or not (a.list or a.extract or a.waves):
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

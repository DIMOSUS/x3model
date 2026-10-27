"""X3: Albion Prelude scene files: binary CUT1 (.pbb/.bob) and text scenes (.pbd/.bod).

Reference reader/writer for both forms into one tree (Scene -> Part -> Key), written from the
engine's own loaders in X3AP.exe (see docs/scene_format.md; src/scene*.cpp of x3model are C++ copies
of this reader and of evaluate_tracks):

  Scene::LoadCutScene   0x00492970  open "pbb bob pbd bod", payload "CUT" -> binary, else text
  Scene::LoadBinaryCut  0x00490ec0  CUT1 grammar (tag table Mesh::g_Cut1TagTable 0x0054eed0)
  Scene::LoadTextCut    0x00491b10  text grammar (VER:, P/B/C/F/N/K/T/M/L/flag letters, { keys })
  Scene::ReadCutKeyframes 0x00490650 one key record; the flag word selects the fields

Python 3, standard library only.

  python x3scene.py dump <file>                     print the tree
  python x3scene.py tocut1 <in> <out.pbb>           any form -> CUT1
  python x3scene.py totext <in> <out.pbd>           any form -> text scene
  python x3scene.py check <game_dir> [--addon N]    acceptance over every installed member
  python x3scene.py interp <game_dir>               self-check of evaluate() (scene_format.md 9)
"""
import gzip
import os
import struct
import sys

# --------------------------------------------------------------------------------------------
# Tree


class Key:
    """One key record of a part's STAT/{ } list.

    flags  : the key flag word as stored in the file (the loaders OR 0x80 into it, not kept here)
    values : field name -> int, in the order of key_layout(flags). 'i' fields are plain i32
             (Body::ReadI32), 'f' fields are 16.16 fixed (Body::ReadRawI32, text: decimal).
    tok    : text form only: the original token per field (and 'flags'), used by the text writer
             to keep the spelling of numbers that were not changed.
    """
    __slots__ = ("flags", "values", "tok")

    def __init__(self, flags, values=None, tok=None):
        self.flags = flags
        self.values = values if values is not None else {}
        self.tok = tok

    def __eq__(self, o):
        return isinstance(o, Key) and self.flags == o.flags and self.values == o.values

    def __repr__(self):
        return "Key(0x%x, %r)" % (self.flags, self.values)


class Part:
    """One scene part (CUT1 PATH ... /PAT, text P ...).

    index   : P number / PATH u32 (the key other parts' F refers to)
    body    : body reference as written: str (text, CUT1 v>=6) or int (CUT1 v<6)
    mode    : C, i32, copied to node+0x260 (part mode/group word); default 0
    parent  : F, i32, P number of the parent part, -1 = scene root; default -1
    flags   : part flag word. CUT1: the u32 as stored. Text: what the letters produce in the
              engine (start 0x40; c 1, l 2 (+ d 4 / p 8 / s 0x10), f 0x20, j -0x40+0x80, p 0x100,
              s 0x200)
    name    : NAME / N string or None
    constraints : CONS / K list of (i32 part number, 16.16 value), or None when the record is
              absent (an empty CONS 0 /CON record exists in the data and is kept)
    notes   : NOTE / T list of (i32 time, str)
    embedded: CUT1 only: raw bytes "BOB1 ... /BOB" of an embedded body, or None
    inline  : text only: ('{', text) or ('', text) for an inline 'L' body, or None
    matrix  : text only: the 9 fixed values of an 'M' record (read and discarded by the engine)
    keys    : list of Key
    letters : text only: header letters in file order, for the writer (e.g. "CNb")
    """

    def __init__(self, index=0, body="", mode=0, parent=-1, flags=0x40):
        self.index = index
        self.body = body
        self.mode = mode
        self.parent = parent
        self.flags = flags
        self.name = None
        self.constraints = None
        self.notes = []
        self.embedded = None
        self.inline = None
        self.matrix = None
        self.keys = []
        self.letters = None
        self.tok = {}            # text tokens of header fields (index, mode, parent, ...)
        self.fmt = None          # text writer hints (spacing), filled by the text reader


class Scene:
    """A whole scene.

    form    : 'cut1' or 'text' (what it was read from)
    version : CUT1 i32 after INFO / text VER: value
    info    : CUT1 INFO string (None if absent; the engine requires it, see the doc)
    parts   : list of Part
    tail    : CUT1: bytes after /CUT; text: text after the last part (not parsed by the engine)
    head    : text: text before VER: (comments)
    """

    def __init__(self, form="cut1", version=6):
        self.form = form
        self.version = version
        self.info = None
        self.parts = []
        self.tail = b""
        self.head = ""


# --------------------------------------------------------------------------------------------
# Key layout: Scene::ReadCutKeyframes 0x00490650. Kinds: 'i' Body::ReadI32 (text: integer),
# 'f' Body::ReadRawI32 (text: decimal -> 16.16).

TCB = ("tension", "continuity", "bias", "ease_in", "ease_out")


def key_layout(flags):
    f = flags
    out = []
    if not f & 0x10:                                   # position (0x00490650..)
        out += [("x", "i"), ("y", "i"), ("z", "i")]
        if f & 0x4000:                                 # TCB of the position track
            out += [("p_" + n, "f") for n in TCB]
    if not f & 8:                                      # object rotation
        if f & 0x20:
            pass                                       # no rotation read (0x2 -> identity key)
        elif f & 2:                                    # axis-angle: angle (turns), axis (negated)
            out += [("angle", "f"), ("ax", "f"), ("ay", "f"), ("az", "f")]
            if f & 0x8000:
                out += [("r_" + n, "f") for n in TCB]
        elif not f & 0x200:                            # three angles (turns, low 16 bits used)
            out += [("yaw", "f"), ("pitch", "f"), ("roll3", "f")]
    else:                                              # target mode (camera / spot)
        if not f & 0x40:
            out += [("tx", "i"), ("ty", "i"), ("tz", "i")]
        if not f & 0x20:
            out += [("roll", "f")]                     # stored negated
    if f & 0x200 and not f & 0x400:                    # light colour r g b (16.16, 1.0 = 255)
        out += [("r", "f"), ("g", "f"), ("b", "f")]
    if f & 0x800 and not f & 0x1000:                   # field of view, radians 16.16
        out += [("fov", "f")]
    out += [("dt", "i"), ("extra", "i")]               # time to next key; second int is discarded
    return out


# --------------------------------------------------------------------------------------------
# Binary CUT1

CUT_TAGS = {  # Mesh::g_Cut1TagTable 0x0054eed0: index -> (tag, closer)
    0: (b"DATA", b"/DAT"), 1: (b"CUT1", b"/CUT"), 2: (b"NAME", b"/NAM"), 3: (b"VERS", b"/VER"),
    4: (b"PATH", b"/PAT"), 5: (b"STAT", b"/STA"), 6: (b"NOTE", b"/NOT"), 7: (b"CONS", b"/CON"),
    8: (b"BOB1", b"/BOB"), 9: (b"INFO", b"/INF"),
}


class FormatError(Exception):
    pass


class _BR:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u32(self):
        if self.p + 4 > len(self.d):
            raise FormatError("read past end at 0x%x" % self.p)
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = self.u32()
        return v - (1 << 32) if v & 0x80000000 else v

    def cstr(self):
        e = self.d.find(b"\0", self.p)
        if e < 0:
            raise FormatError("unterminated string at 0x%x" % self.p)
        s = self.d[self.p:e].decode("latin-1")
        self.p = e + 1
        return s

    def peek(self, tag):
        return self.d[self.p:self.p + 4] == tag

    def tag(self, tag):
        if not self.peek(tag):
            raise FormatError("expected %r at 0x%x, got %r" % (tag, self.p, self.d[self.p:self.p + 4]))
        self.p += 4


def read_cut1(data):
    r = _BR(data)
    r.tag(b"CUT1")
    sc = Scene("cut1")
    if r.peek(b"INFO"):                     # 0x00490ff9: ReadTag(1) == 9 (INFO), else error flag
        r.tag(b"INFO")
        sc.info = r.cstr()
        r.tag(b"/INF")
    sc.version = r.i32()
    n = r.i32()
    for _ in range(n):
        r.tag(b"PATH")
        p = Part()
        p.index = r.u32()
        p.body = r.i32() if sc.version < 6 else r.cstr()
        p.mode = r.i32()
        p.parent = r.i32()
        p.flags = r.u32()
        if r.peek(b"NAME"):
            r.tag(b"NAME")
            p.name = r.cstr()
            r.tag(b"/NAM")
        if r.peek(b"CONS"):
            r.tag(b"CONS")
            p.constraints = []
            for _ in range(r.i32()):
                p.constraints.append((r.i32(), r.i32()))
            r.tag(b"/CON")
        if r.peek(b"BOB1"):
            # 0x0049151c: the engine parses the body from the tag, then Body::SeekTagInStream
            # (0x004e9e20) scans forward byte by byte for the first "/BOB" after the tag.
            start = r.p
            e = data.find(b"/BOB", start + 4)
            if e < 0:
                raise FormatError("embedded body without /BOB")
            p.embedded = data[start:e + 4]
            r.p = e + 4
        nn = r.i32()
        if nn:
            r.tag(b"NOTE")
            for _ in range(nn):
                p.notes.append((r.i32(), r.cstr()))
            r.tag(b"/NOT")
        nk = r.i32()
        if nk:
            r.tag(b"STAT")
            for _ in range(nk):
                k = Key(r.u32())
                for name, _kind in key_layout(k.flags):
                    k.values[name] = r.i32()
                p.keys.append(k)
            r.tag(b"/STA")
        r.tag(b"/PAT")
        sc.parts.append(p)
    r.tag(b"/CUT")
    sc.tail = data[r.p:]
    return sc


def _s32(v):
    return struct.pack(">i", v)


def _u32(v):
    return struct.pack(">I", v & 0xffffffff)


def _cstr(s):
    if isinstance(s, int):
        s = str(s)
    return s.encode("latin-1") + b"\0"


def write_cut1(sc, version=None):
    """Tree -> CUT1 bytes. A text tree is converted: flags kept as the engine computes them,
    inline text bodies are dropped (CUT1 embeds only BOB1 bodies; the part keeps its body name)."""
    v = sc.version if version is None else version
    if sc.form == "text" and version is None:
        v = 6
    o = [b"CUT1"]
    if sc.info is not None:
        o += [b"INFO", _cstr(sc.info), b"/INF"]
    o += [_s32(v), _s32(len(sc.parts))]
    for p in sc.parts:
        o += [b"PATH", _u32(p.index)]
        if v < 6:
            o.append(_s32(int(p.body)))
        else:
            o.append(_cstr(p.body))
        o += [_s32(p.mode), _s32(p.parent), _u32(p.flags)]
        if p.name is not None:
            o += [b"NAME", _cstr(p.name), b"/NAM"]
        if p.constraints is not None:
            o += [b"CONS", _s32(len(p.constraints))]
            for a, b in p.constraints:
                o += [_s32(a), _s32(b)]
            o.append(b"/CON")
        if p.embedded is not None:
            o.append(p.embedded)
        o.append(_s32(len(p.notes)))
        if p.notes:
            o.append(b"NOTE")
            for t, s in p.notes:
                o += [_s32(t), _cstr(s)]
            o.append(b"/NOT")
        o.append(_s32(len(p.keys)))
        if p.keys:
            o.append(b"STAT")
            for k in p.keys:
                o.append(_u32(k.flags))
                for name, _kind in key_layout(k.flags):
                    o.append(_s32(k.values[name]))
            o.append(b"/STA")
        o.append(b"/PAT")
    o.append(b"/CUT")
    o.append(sc.tail if isinstance(sc.tail, bytes) else b"")
    return b"".join(o)


# --------------------------------------------------------------------------------------------
# Text lexer: a faithful copy of the engine's text primitives (Body::TextSkipWhitespace 0x004e98a0,
# TextParseInt 0x004eb930, TextParseFixed16 0x004ec490, TextReadToken 0x004e9930,
# TextReadCharInSet 0x004ea160). The buffer is bytes; NUL ends it.


class _TR:
    def __init__(self, text, strict=True):
        self.d = text + b"\0"
        self.p = 0
        self.err = False
        self.strict = strict     # False: accept ',' as decimal point and '#...' (NaN) as 0

    def c(self, k=0):
        i = self.p + k
        return self.d[i] if i < len(self.d) else 0

    def skip_ws(self):
        while True:
            c = self.c()
            if c in (9, 10, 13, 32):
                self.p += 1
            elif c == 0x2f:                                    # '/' comment to end of line
                while self.c() not in (10, 0):
                    self.p += 1
            else:
                return

    def _comment(self):
        while self.c() not in (10, 0):
            self.p += 1

    def parse_int(self):
        """TextParseInt: '-', '%' binary, '0x' hex, decimal; ends at ';' (consumed) or NUL;
        whitespace inside is skipped; '#...;' = 0."""
        base, val, neg = 0, 0, False
        while True:
            c = self.c()
            if c in (0, 0x3b):
                if base == 0:
                    self.err = True
                    return 0
                if c == 0x3b:
                    self.p += 1
                v = -val if neg else val
                v &= 0xffffffff
                return v - (1 << 32) if v & 0x80000000 else v
            if c in (9, 10, 13, 32):
                self.p += 1
                continue
            if c == 0x23:                                      # '#'
                if base:
                    self.err = True
                    return 0
                while self.c() not in (0x3b, 0):
                    self.p += 1
                if self.c() == 0x3b:
                    self.p += 1
                return 0
            if c == 0x25:                                      # '%'
                if base:
                    self.err = True
                    return 0
                base = 2
                self.p += 1
                continue
            if c == 0x2d:                                      # '-'
                if base or neg:
                    self.err = True
                    return 0
                neg = True
                self.p += 1
                continue
            if c == 0x2f:
                self._comment()
                continue
            if c == 0x30 and base == 0 and self.c(1) in (0x78, 0x58):
                base = 16
                self.p += 2
                continue
            if base == 2:
                if neg or c not in (0x30, 0x31):
                    self.err = True
                    return 0
                val = val * 2 + (c - 0x30)
            elif base == 16:
                ch = chr(c)
                if neg or ch not in "0123456789abcdefABCDEF":
                    self.err = True
                    return 0
                val = val * 16 + int(ch, 16)
            else:
                if not 0x30 <= c <= 0x39:
                    self.err = True
                    return 0
                base = 10
                val = val * 10 + (c - 0x30)
            val &= 0xffffffff
            self.p += 1

    def parse_fixed(self):
        """TextParseFixed16: decimal accumulated in a double (int part v*10+d, fraction
        scale*=0.1), then trunc(v*65536). Precision: double (assumes the x87 53-bit mode)."""
        val, scale, neg, started = 0.0, 0.0, False, False
        while True:
            c = self.c()
            if c in (0, 0x3b):
                if not started:
                    self.err = True
                    return 0
                if c == 0x3b:
                    self.p += 1
                if neg:
                    val = -val
                x = val * 65536.0
                try:
                    v = int(x)                                 # _ftol truncates toward zero
                except (OverflowError, ValueError):
                    v = -0x80000000
                if not -0x80000000 <= v <= 0x7fffffff:
                    v = -0x80000000
                return v
            if c in (9, 10, 13, 32):
                self.p += 1
                continue
            if c == 0x23:
                if started and not self.strict:
                    while self.c() not in (0x3b, 0):
                        self.p += 1
                    if self.c() == 0x3b:
                        self.p += 1
                    return 0
                if started:
                    self.err = True
                    return 0
                while True:                                    # stops AT the ';' (not consumed)
                    if self.c() == 0:
                        return 0
                    self.p += 1
                    if self.c() == 0x3b:
                        return 0
            if c == 0x2d:
                if started:
                    self.err = True
                    return 0
                neg = True
                self.p += 1
                continue
            if c == 0x2e or (c == 0x2c and not self.strict):
                if scale != 0.0:
                    self.err = True
                    return 0
                scale = 1.0
                self.p += 1
                continue
            if c == 0x2f:
                self._comment()
                continue
            if not 0x30 <= c <= 0x39:
                self.err = True
                return 0
            started = True
            if scale == 0.0:
                val = (float(c) + val * 10.0) - 48.0
            else:
                scale = scale * 0.1
                val = float(c - 0x30) * scale + val
            self.p += 1

    def token(self, skip_spaces=False):
        """TextReadToken: up to ';' (consumed) or NUL; CR/LF/TAB dropped; '/' comment."""
        out = bytearray()
        while True:
            c = self.c()
            if c in (0, 0x3b):
                if c == 0x3b:
                    self.p += 1
                return out.decode("latin-1")
            if c in (9, 10, 13):
                self.p += 1
                continue
            if c == 32 and skip_spaces:
                self.p += 1
                continue
            if c == 0x2f:
                self._comment()
                continue
            out.append(c)
            self.p += 1

    # engine-level readers
    def read_i32(self):
        if self.err:
            return 0, ""
        self.skip_ws()
        s = self.p
        v = self.parse_int()
        return v, self.d[s:self.p].decode("latin-1")

    def read_fixed(self):
        if self.err:
            return 0, ""
        self.skip_ws()
        s = self.p
        v = self.parse_fixed()
        return v, self.d[s:self.p].decode("latin-1")

    def read_string(self):
        if self.err:
            return ""
        self.skip_ws()
        return self.token(False)

    def char_in(self, cs):
        """TextReadCharInSet: skip whitespace, return the char; consume it only if in cs."""
        if self.err:
            return 0
        self.skip_ws()
        c = self.c()
        if chr(c) in cs and c != 0:
            self.p += 1
        return c


HEADER_SET = "CFENKMLTclbjfsp"
TEXT_LABELS = ["NAME:", "VER:", "PATH:", "STATES:", "NOTES:", "POSCONSTRAINT:", "BODY:", "INFO:"]


def _tok_strip(t):
    t = t.strip()
    return t[:-1].rstrip() if t.endswith(";") else t


def read_text(data, strict=True):
    """Text scene -> tree, following Scene::LoadTextCut 0x00491b10 step by step.
    strict=True fails exactly where the engine's parser sets its error flag."""
    if isinstance(data, str):
        data = data.encode("latin-1")
    r = _TR(data, strict)
    sc = Scene("text")
    r.skip_ws()
    sc.head = data[:r.p].decode("latin-1")
    lab = None
    for i, l in enumerate(TEXT_LABELS):          # Body::ReadTag text mode: first label that matches
        if data[r.p:r.p + len(l)] == l.encode():
            lab = l
            break
    if lab != "VER:":
        raise FormatError("text scene does not start with VER: (%r)" % data[r.p:r.p + 12])
    r.p += len(lab)
    sc.version, _ = r.read_i32()
    if r.err:
        raise FormatError("bad VER: value")
    c = r.char_in("P")
    while c == ord("P") and not r.err:
        p = Part(flags=0x40)
        p.body = None
        p.index, p.tok["index"] = r.read_i32()
        c = r.char_in("B")
        if c != ord("B"):
            # 0x0049279b: error flag, the parts read so far are kept and the scene is returned
            sc.tail = data[r.p:].decode("latin-1")
            r.err = True
            break
        p.body = r.read_string()
        letters = []
        while True:
            c = r.char_in(HEADER_SET)
            if not c or r.err or chr(c) not in HEADER_SET or chr(c) == "E":
                if c == ord("E"):
                    letters.append("E")
                break
            ch = chr(c)
            letters.append(ch)
            if ch == "C":
                p.mode, p.tok["mode"] = r.read_i32()
            elif ch == "F":
                p.parent, p.tok["parent"] = r.read_i32()
            elif ch == "K":
                n, _ = r.read_i32()
                p.constraints = []
                for _ in range(n if n > 0 else 0):
                    a, _ = r.read_i32()
                    b, _ = r.read_fixed()
                    p.constraints.append((a, b))
            elif ch == "L":
                c2 = r.char_in("{")
                if c2 != ord("{"):
                    e = data.find(b"{", r.p)
                    if e < 0:
                        e = len(data)
                    p.inline = ("", data[r.p:e].decode("latin-1"))
                    r.p = e
                else:
                    e = data.find(b"}", r.p)
                    if e < 0:
                        raise FormatError("inline body without }")
                    p.inline = ("{", data[r.p:e].decode("latin-1"))
                    r.p = e + 1
            elif ch == "M":
                p.matrix = [r.read_fixed()[0] for _ in range(9)]
            elif ch == "N":
                p.name = r.read_string()
            elif ch == "T":
                p.notes = []
                v, _ = r.read_i32()
                while v != -1 and not r.err:
                    if len(p.notes) > 99:
                        raise FormatError("more than 100 notes")
                    p.notes.append((v, r.read_string()))
                    v, _ = r.read_i32()
            elif ch == "b":
                pass
            elif ch == "c":
                p.flags |= 1
            elif ch == "f":
                p.flags |= 0x20
            elif ch == "j":
                p.flags = (p.flags & ~0x40) | 0x80
            elif ch == "l":
                p.flags |= 2
                if r.char_in("d") == ord("d"):
                    p.flags |= 4
                    letters.append("d")
                elif r.char_in("p") == ord("p"):
                    p.flags |= 8
                    letters.append("p")
                elif r.char_in("s") == ord("s"):
                    p.flags |= 0x10
                    letters.append("s")
            elif ch == "p":
                p.flags |= 0x100
            elif ch == "s":
                p.flags |= 0x200
        p.letters = "".join(letters)
        if r.err:
            raise FormatError("part P %s: header parse error at %d" % (p.index, r.p))
        while True:
            c = r.char_in("{")
            if c != ord("{") or r.err:
                break
            if sc.version < 2:
                flags, ftok = 1, None
            else:
                flags, ftok = r.read_i32()
            k = Key(flags, {}, {"flags": ftok})
            for name, kind in key_layout(flags):
                v, t = r.read_i32() if kind == "i" else r.read_fixed()
                k.values[name] = v
                k.tok[name] = t
            if r.err:
                raise FormatError("part P %s: key %d parse error at %d" % (p.index, len(p.keys), r.p))
            if r.char_in("}") != ord("}"):
                raise FormatError("part P %s: key %d not closed by } at %d" % (p.index, len(p.keys), r.p))
            p.keys.append(k)
        sc.parts.append(p)
        c = r.char_in("P")
    if not r.err:
        sc.tail = data[r.p:].decode("latin-1")
    return sc


def _fixed_str(v):
    """16.16 -> decimal text that the engine's parser (truncating) turns back into v."""
    s = "%.6f" % (v / 65536.0)
    if _TR(s.encode() + b";").parse_fixed() == v:
        return s
    # aim at the middle of [v, v+1) (away from zero), which survives the truncation
    mid = (v + (0.5 if v >= 0 else -0.5)) / 65536.0
    for nd in range(6, 13):
        s = "%.*f" % (nd, mid)
        if _TR(s.encode() + b";").parse_fixed() == v:
            return s
    raise ValueError("no decimal form for 16.16 value %d" % v)


def write_text(sc):
    """Tree -> text scene. Keeps the original number spelling when the value is unchanged;
    comments other than the head are not kept."""
    out = []
    head = sc.head if sc.form == "text" else "/ converted by x3scene.py\n"
    out.append(head)
    out.append("VER: %d;\n" % (sc.version if sc.form == "text" else 6))
    for p in sc.parts:
        def tok(name, v):
            t = p.tok.get(name)
            return _tok_strip(t) if t is not None and _TR(t.encode()).parse_int() == v else str(v)
        line = ["P %s;" % tok("index", p.index), " B %s;" % p.body]
        letters = p.letters
        if letters is None:                                  # tree from CUT1: derive the letters
            letters = ""
            if p.mode != 0:
                letters += "C"
            if p.parent != -1:
                letters += "F"
            if p.name is not None:
                letters += "N"
            if p.constraints is not None:
                letters += "K"
            if p.notes:
                letters += "T"
            f = p.flags
            if f & 0x80 and not f & 0x40:
                letters += "j"
            if f & 1:
                letters += "c"
            if f & 2:
                letters += "l" + ("d" if f & 4 else "p" if f & 8 else "s" if f & 0x10 else "")
            if f & 0x20:
                letters += "f"
            if f & 0x100:
                letters += "p"
            if f & 0x200:
                letters += "s"
            letters += "b"
        i = 0
        while i < len(letters):
            ch = letters[i]
            if ch == "C":
                line.append(" C %s;" % tok("mode", p.mode))
            elif ch == "F":
                line.append(" F %s;" % tok("parent", p.parent))
            elif ch == "N":
                line.append(" N %s;" % p.name)
            elif ch == "K":
                line.append(" K %d;" % len(p.constraints) +
                            "".join(" %d; %s;" % (a, _fixed_str(b)) for a, b in p.constraints))
            elif ch == "T":
                line.append(" T" + "".join(" %d; %s;" % (t, s) for t, s in p.notes) + " -1;")
            elif ch == "M":
                line.append(" M" + "".join(" %s;" % _fixed_str(v) for v in p.matrix))
            elif ch == "L":
                br, txt = p.inline
                line.append(" L {%s}" % txt if br else " L %s" % txt)
            elif ch == "l" and i + 1 < len(letters) and letters[i + 1] in "dps" and \
                    p.flags & {"d": 4, "p": 8, "s": 0x10}[letters[i + 1]]:
                line.append(" l" + letters[i + 1])
                i += 1
            else:
                line.append(" " + ch)
            i += 1
        out.append("".join(line) + "\n")
        for k in p.keys:
            vals = []
            if sc.form != "text" or sc.version >= 2:
                ft = (k.tok or {}).get("flags")
                fl = _tok_strip(ft) if ft is not None and _TR(ft.encode()).parse_int() == k.flags \
                    else "0x%x" % k.flags
                vals.append(fl)
            for name, kind in key_layout(k.flags):
                v = k.values[name]
                t = (k.tok or {}).get(name)
                if t is not None:
                    tv = _TR(t.encode()).parse_int() if kind == "i" else _TR(t.encode()).parse_fixed()
                    if tv == v:
                        vals.append(_tok_strip(t))
                        continue
                vals.append(str(v) if kind == "i" else _fixed_str(v))
            out.append("  { " + "; ".join(vals) + "; }\n")
    if sc.form == "text":
        out.append(sc.tail)
    return "".join(out).encode("latin-1")


# --------------------------------------------------------------------------------------------
# Common entry points


def read(data, strict=True):
    """Bytes of a decoded scene member -> tree (the choice of Scene::LoadCutScene: "CUT" prefix)."""
    if data[:3] == b"CUT":
        return read_cut1(data)
    return read_text(data, strict)


def norm_parts(sc):
    """Engine-level view of a tree for comparing a CUT1 and a text scene: per part
    (index, body name, mode, parent, flags, name, constraints, notes, keys)."""
    out = []
    for p in sc.parts:
        body = str(p.body)
        fl = p.flags
        if sc.form == "cut1":   # 0x0049137b..: v<6 or no 0x80 -> the loader ORs 0x40 (body is a model)
            if sc.version < 6 or not fl & 0x80:
                fl |= 0x40
        out.append({
            "index": p.index, "body": body.lower().replace("/", "\\"), "mode": p.mode,
            "parent": p.parent, "flags": fl, "name": p.name,
            "constraints": list(p.constraints or []), "notes": list(p.notes),
            "keys": [(k.flags | 0x80, dict(k.values)) for k in p.keys],
            "embedded": p.embedded is not None or p.inline is not None,
        })
    return out


def compare(a, b, tol=1, embedded=True):
    """Compare two trees part by part. Returns (verdict, details). 16.16 fields are allowed to
    differ by tol (rounding of the decimal text), plain ints must match. embedded=False ignores
    whether a part carries an embedded/inline body."""
    pa, pb = norm_parts(a), norm_parts(b)
    det = []
    if len(pa) != len(pb):
        return "different", ["part count %d vs %d" % (len(pa), len(pb))]
    exact = True
    fields = ("index", "body", "mode", "parent", "flags", "name", "notes") + \
        (("embedded",) if embedded else ())
    for x, y in zip(pa, pb):
        for f in fields:
            if x[f] != y[f]:
                det.append("P %s %s: %r vs %r" % (x["index"], f, x[f], y[f]))
        if len(x["constraints"]) != len(y["constraints"]):
            det.append("P %s constraints count" % x["index"])
        if len(x["keys"]) != len(y["keys"]):
            det.append("P %s key count %d vs %d" % (x["index"], len(x["keys"]), len(y["keys"])))
            continue
        for i, ((fa, va), (fb, vb)) in enumerate(zip(x["keys"], y["keys"])):
            if fa != fb:
                det.append("P %s key %d flags 0x%x vs 0x%x" % (x["index"], i, fa, fb))
                continue
            kinds = dict(key_layout(fa & ~0x80))
            for n in va:
                if va[n] != vb[n]:
                    if kinds[n] == "f" and abs(va[n] - vb[n]) <= tol:
                        exact = False
                    else:
                        det.append("P %s key %d %s: %d vs %d" % (x["index"], i, n, va[n], vb[n]))
    if det:
        return "different", det
    return ("same" if exact else "same within 1/65536"), det


# --------------------------------------------------------------------------------------------
# Engine semantics helpers for converters (floating point; the engine works in 16.16)


def key_times(part):
    """Time of every key: 0 for the first, then the running sum of the previous keys' dt
    (Scene::ReadCutKeyframes: key time = g_CutKeyTime, then g_CutKeyTime += dt). Milliseconds
    (inferred, see scene_format.md)."""
    t, out = 0, []
    for k in part.keys:
        out.append(t)
        t += k.values["dt"]
    return out


def _qmul(a, b):
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return (w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 + y1 * w2 + z1 * x2 - x1 * z2, w1 * z2 + z1 * w2 + x1 * y2 - y1 * x2)


def rotation_quats(part):
    """Per key: the quaternion (w, x, y, z) the engine builds for axis-angle keys (flag 0x2), or
    None for keys of another rotation kind / without rotation.
    Scene::ReadCutKeyframes stores the axis negated; Scene::BuildRotationKeyQuats turns
    (angle in turns, -axis) into a quaternion (Math::AxisAngleToQuatFixed: half angle, zero axis =
    identity) and, unless the key has flag 0x2000, composes it with the previous result:
    q = q_prev (x) q_key (Math::QuatMultiplyFixed operand order), normalised as the engine does
    (an absolute key with a non-unit axis stays non-unit, as in the engine). A key with 0x20 and 0x2 is an
    identity key; keys with 0x20 alone or without 0x2 do not change the running quaternion."""
    import math
    run = (1.0, 0.0, 0.0, 0.0)
    out = []
    for k in part.keys:
        f, v = k.flags, k.values
        if f & 8 or not f & 2:
            out.append(None)
            continue
        if f & 0x20:
            q = (1.0, 0.0, 0.0, 0.0)
        else:
            ax, ay, az = -v["ax"] / 65536.0, -v["ay"] / 65536.0, -v["az"] / 65536.0
            if ax == ay == az == 0:
                q = (1.0, 0.0, 0.0, 0.0)
            else:
                h = (_cdiv(v["angle"], 2) & 0xffff) / 65536.0 * 2 * math.pi   # half, C division
                s = math.sin(h)
                q = (math.cos(h), ax * s, ay * s, az * s)
        if not f & 0x2000:
            q = _qmul(run, q)
            n = math.sqrt(sum(c * c for c in q))           # QuatMultiplyFixed normalises
            q = tuple(c / n for c in q)
        run = q
        out.append(q)
    return out


# --------------------------------------------------------------------------------------------
# Engine interpolation (scene_format.md section 9). Mirrors Scene::AnimateNode 0x0048dc20 and
# the key preparation of Scene::CommitCutArrays 0x004901a0 in 16.16 fixed point; the engine's
# x87 code (80-bit) is done in doubles here, so a result can differ by 1/65536 in rare cases.


def _i32(v):
    """Wrap to a signed 32-bit int (register width)."""
    v &= 0xffffffff
    return v - 0x100000000 if v & 0x80000000 else v


def _r16(a, b):
    """round16(a*b): IMUL; ADD 0x8000; ADC 0; SHRD 16 (arithmetic shift, half up)."""
    return _i32((a * b + 0x8000) >> 16)


def _cdiv(a, b):
    """C / IDIV division, truncating toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def _fixdiv(a, b):
    """Math::FixedDivide 0x00412450: trunc(a*65536/b), 0 for b == 0."""
    return 0 if b == 0 else _i32(_cdiv(a << 16, b))


def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _scale(k, v):
    """Math::Vec3ScaleFixed 0x0040e780: round16(k * v[i])."""
    return [_r16(c, k) for c in v]


def _add(a, b):
    return [x + y for x, y in zip(a, b)]


def _sub(a, b):
    return [x - y for x, y in zip(a, b)]


_TWO_PI_F = 6.2831854820251465          # float 2*pi (Math::g_TwoPi; DAT_00565528 = this/65536)
_PI_F_NUM = 13176795                    # float pi (DAT_0056568c, DAT_00565698) = 13176795 / 2**22
_SIN = None
_ATAN = None


def _tables():
    """Math::BuildAsinTable 0x004f0110: sine T[k] = trunc(sin(k*float(2pi)/65536)*65536) for
    k 0..0x4000 (so T[0x4000] = 65535); Math::g_AtanTable A[i] = trunc(atan(i/65536)/(8*atan(1))
    *65536) for i 0..0x10000 (FISTP with the chop rounding mode, 0..0x2000)."""
    global _SIN, _ATAN
    if _SIN is None:
        import math
        c = _TWO_PI_F / 65536
        _SIN = [int(math.sin(k * c) * 65536.0) for k in range(0x4001)]
        q = math.atan2(1.0, 1.0) * 8.0
        _ATAN = [int(math.atan(i / 65536.0) / q * 65536.0) for i in range(0x10001)]
    return _SIN, _ATAN


def _sin16(u):
    """Quadrant lookup (AxisAngleToQuatFixed, AnimateNode) of a 16-bit angle."""
    t = _tables()[0]
    u &= 0xffff
    if u <= 0x4000:
        return t[u]
    if u <= 0x8000:
        return t[0x8000 - u]
    if u <= 0xc000:
        return -t[u - 0x8000]
    return -t[0x10000 - u]


def _cos16(u):
    t = _tables()[0]
    u &= 0xffff
    if u <= 0x4000:
        return t[0x4000 - u]
    if u <= 0x8000:
        return -t[u - 0x4000]
    if u <= 0xc000:
        return -t[0xc000 - u]
    return t[u - 0xc000]


def _muldiv(a, b, c):
    """Win32 MulDiv: a*b/c rounded half away from zero, -1 on overflow."""
    if c == 0:
        return -1
    if c < 0:
        a, c = -a, -c
    if (a < 0) != (b < 0):
        r = _cdiv(a * b - c // 2, c)
    else:
        r = _cdiv(a * b + c // 2, c)
    return r if -2147483647 <= r <= 2147483647 else -1


def _lerp_int(t, dur, end, start):
    """Math::LerpInt 0x00469de0 (t from the segment start, dur = segment length)."""
    if dur == 0:
        return _cdiv(start + end, 2)
    if t < 1:
        return start
    if dur <= t:
        return end
    if t < 0x10000 and dur < 0x10000:
        return _cdiv(start + end, 2) + _muldiv(end - start, t * 2 - dur, dur * 2)
    return _cdiv(start + end, 2) + _cdiv((end - start) * (t - _cdiv(dur, 2)), dur)


def _find_key(times, t):
    """Scene::FindKeyframeIndex 0x00496d30 (also ...12/...20): last index when t >= last time,
    0 when t <= first, else bisection to i with times[i] <= t < times[i+1]."""
    hi = len(times) - 1
    if times[hi] <= t:
        return hi
    if not times[0] < t:
        return 0
    lo = mid = 0
    if hi != 0:
        while True:
            if lo + 1 == hi and t < times[hi]:
                return lo
            mid = (hi + lo) // 2
            if t < times[mid]:
                hi, mid = mid, lo
            lo = mid
            if mid == hi:
                break
    return mid


def _ease(t, ein, eout):
    """Scene::TcbEaseCurve 0x0048cf60(t, ein, eout), 16.16. The callers pass ein = ease out
    (+0x18) of the segment's start key and eout = ease in (+0x14) of its end key."""
    s = ein + eout
    if s == 0:
        return t
    if s > 0x10000:
        ein, eout = _fixdiv(ein, s), _fixdiv(eout, s)
    d = 0x20000 - ein - eout
    k = 0 if d == 0 else _cdiv(1 << 32, d)              # 1/(2-in-out) in 16.16
    if t < ein:
        return _i32(_cdiv(k * _r16(t, t), ein))
    if t < 0x10000 - eout:
        return _r16(k, t * 2 - ein)
    if eout == 0:
        return 0x10000
    return 0x10000 - _i32(_cdiv(k * _r16(0x10000 - t, 0x10000 - t), eout))


def _qmul_fixed(q2, q1):
    """Math::QuatMultiplyFixed 0x004f2640(q2, q1): q1 (x) q2 rounded per product, then
    Math::QuatNormalizeFixed 0x004f0020 (floating scale, components truncated)."""
    w1, x1, y1, z1 = q1
    w2, x2, y2, z2 = q2
    w = _r16(w1, w2) - _r16(z1, z2) - _r16(y1, y2) - _r16(x1, x2)
    x = _r16(y1, z2) - _r16(z1, y2) + _r16(x1, w2) + _r16(w1, x2)
    y = _r16(z1, x2) - _r16(x1, z2) + _r16(y1, w2) + _r16(w1, y2)
    z = _r16(x1, y2) - _r16(y1, x2) + _r16(z1, w2) + _r16(w1, z2)
    if w or x or y or z:
        k = 1.0 / ((float(x) * x + float(y) * y + float(z) * z + float(w) * w) ** 0.5
                   * 1.52587890625e-05)
        w, x, y, z = int(w * k), int(x * k), int(y * k), int(z * k)
    return [w, x, y, z]


def _axis_angle_quat(angle, ax, ay, az):
    """Math::AxisAngleToQuatFixed 0x004f2b50: zero axis -> identity; half = (angle/2) & 0xffff
    (C division), w = cos16(half), xyz = round16(axis * sin16(half)); the axis is not normalised."""
    if ax == 0 and ay == 0 and az == 0:
        return [0x10000, 0, 0, 0]
    h = _cdiv(angle, 2) & 0xffff
    s = _sin16(h)
    return [_cos16(h), _r16(ax, s), _r16(ay, s), _r16(az, s)]


def _slerp(qf, qt, u):
    """Math::QuatSlerp 0x004f2920(to=ECX, out=ESI, from=EDI, u): from -> to by u (16.16); dot < 0
    takes the short arc (the 'to' term is subtracted), |dot| clamped to 1, acos/sin in floating
    point, components truncated; sin(theta) == 0 returns 'from'."""
    import math
    fu = u * 1.52587890625e-05
    f = [c * 1.52587890625e-05 for c in qf]
    g = [c * 1.52587890625e-05 for c in qt]
    dot = f[1] * g[1] + f[2] * g[2] + f[3] * g[3] + f[0] * g[0]
    neg = False
    if dot > 1.0:
        c = 1.0
    elif dot < -1.0:
        c, neg = 1.0, True
    elif dot < 0.0:
        c, neg = -dot, True
    else:
        c = dot
    th = math.acos(c)
    s = math.sin(th)
    if s == 0.0:
        return list(qf)
    r = 1.0 / s
    k0 = math.sin((1.0 - fu) * th) * r
    k1 = math.sin(th * fu) * r
    if neg:
        k1 = -k1
    return [int((k0 * a + k1 * b) * 65536.0) for a, b in zip(f, g)]


def _quat_matrix(q):
    """Math::QuatToMatrixRows 0x004f2300: the node matrix rows (16.16), applied to row vectors."""
    w, x, y, z = q
    xx, yy, zz = _r16(x, x), _r16(y, y), _r16(z, z)
    xy, xz, yz = _r16(x, y), _r16(x, z), _r16(y, z)
    wx, wy, wz = _r16(w, x), _r16(w, y), _r16(w, z)
    a = 0x8000 - zz
    return [[(a - yy) * 2, (xy - wz) * 2, (wy + xz) * 2],
            [(wz + xy) * 2, (a - xx) * 2, (yz - wx) * 2],
            [(xz - wy) * 2, (wx + yz) * 2, ((0x8000 - yy) - xx) * 2]]


def _angles_matrix(a, b, c):
    """Math::AnglesToMatrix 0x004f0270(m, a, b, c), each angle & 0xffff (16-bit turns, times
    float 2pi): a = yaw (about Y), b = pitch (about X), c = roll (about Z); for row vectors
    M = Rz(c) Rx(b) Ry(a) (roll applied first), row 2 (local +Z) = (-sin a cos b, sin b,
    cos a cos b). The (float) roundings are the engine's FST float spills."""
    import math
    k = 1.52587890625e-05 * _TWO_PI_F
    A, B, C = (a & 0xffff) * k, (b & 0xffff) * k, (c & 0xffff) * k
    ca, sa, cb, sb = math.cos(A), math.sin(A), math.cos(B), math.sin(B)
    cc, sc = math.cos(C), math.sin(C)
    F = 65536.0
    return [[int((cc * ca + sc * sb * sa) * F), int(sc * cb * F),
             int((cc * sa - _f32(sc * sb) * ca) * F)],
            [int((_f32(cc * sb) * sa - sc * ca) * F), int(cc * cb * F),
             int((-(sc * sa) - _f32(cc * sb) * ca) * F)],
            [int(-(sa * cb) * F), int(_f32(sb) * F), int(F * cb * ca)]]


class _K:
    """One engine key record (0x50 bytes) of a position, rotation or target track."""
    __slots__ = ("t", "f", "tcb", "v", "a", "b")

    def __init__(self, t, f):
        self.t, self.f = t, f
        self.tcb = [0, 0, 0, 0, 0]       # +0x08..+0x18 T C B ease-in ease-out; 0 unless read
        self.v = [0, 0, 0]               # +0x20..+0x28 position / target / angle (+0x20)
        self.a = [0, 0, 0]               # +0x30..+0x38 incoming tangent / axis (negated)
        self.b = [0, 0, 0, 0]            # +0x40..+0x4c outgoing tangent / quaternion (w,x,y,z)


def _tcb_inner(p, n, k):
    """Scene::ComputeTcbKeyTangents 0x0048f850(prev, next, cur): Kochanek-Bartels tangents with
    the time-interval adjustment (in: *(t-tp)/(tn-tp), out: *(tn-t)/(tn-tp))."""
    T, C, B = k.tcb[0], k.tcb[1], k.tcb[2]
    in_a = _r16(_r16(0x10000 - T, 0x10000 - C), B + 0x10000)      # on cur - prev
    in_b = _r16(_r16(0x10000 - T, C + 0x10000), 0x10000 - B)      # on next - cur
    out_a = _r16(_r16(0x10000 - T, C + 0x10000), B + 0x10000)     # on cur - prev
    out_b = _r16(_r16(0x10000 - T, 0x10000 - C), 0x10000 - B)     # on next - cur
    r_in = _fixdiv(k.t - p.t, n.t - p.t)
    r_out = _fixdiv(n.t - k.t, n.t - p.t)
    d_in, d_out = _sub(k.v, p.v), _sub(n.v, k.v)
    k.a = _scale(r_in, _add(_scale(in_b, d_out), _scale(in_a, d_in)))
    k.b = _scale(r_out, _add(_scale(out_b, d_out), _scale(out_a, d_in))) + [0]


def _tcb_end(D, d, dt_near, dt_far, tension):
    """Scene::ComputeTcbStartTangent 0x0048fb60 / ComputeTcbEndTangent 0x0048fcd0 (identical
    arithmetic): D = far - end key (start: p2-p0; end: pL-p(n-3)), d = the near difference,
    r = dt_near / (2 dt_far); tangent = D/2 + 1.5 (d - D/2) + (1-T) (1/4 - r) D."""
    den = dt_far * 2
    r = _fixdiv(dt_near, den) if den else 0
    c0, c2 = 0x4000 - r, 0x10000 - tension
    s = _scale(0x18000, _sub(d, _scale(0x8000, D)))
    return _add(_scale(0x8000, D), _add(_scale(c2, _scale(c0, D)), s))


def engine_tracks(part):
    """The key tracks the engine builds for a part: Scene::ReadCutKeyframes 0x00490650 (which
    record goes to which track; times = running sum of dt), BuildRotationKeyQuats 0x0048f760
    (quaternions) and ComputeTcbTangentsForKeys 0x0048fe40 (position tangents). Returns a dict:
    'pos', 'rot', 'target' -> lists of _K; 'roll', 'fov' -> (time, flags, value);
    'colour' -> (time, flags, [r, g, b])."""
    pos, rot, tgt, roll, col, fov = [], [], [], [], [], []
    t = 0
    for key in part.keys:
        f, v = key.flags | 0x80, key.values
        if not f & 0x10:
            k = _K(t, f)
            k.v = [v["x"], v["y"], v["z"]]
            if f & 0x4000:
                k.tcb = [v["p_" + n] for n in TCB]
            pos.append(k)
        if not f & 8:
            if f & 0x20:
                if f & 2:                                   # identity key, 0x20 cleared
                    k = _K(t, f & ~0x20)
                    k.a = [-0x10000, 0, 0]
                    rot.append(k)
            elif f & 2:
                k = _K(t, f)
                k.v[0] = v["angle"]
                k.a = [_i32(-v["ax"]), _i32(-v["ay"]), _i32(-v["az"])]
                if f & 0x8000:
                    k.tcb = [v["r_" + n] for n in TCB]
                rot.append(k)
            elif not f & 0x200:
                k = _K(t, f)
                k.v = [v["yaw"], v["pitch"], v["roll3"]]
                rot.append(k)
        else:
            if not f & 0x40:
                k = _K(t, f)
                k.v = [v["tx"], v["ty"], v["tz"]]
                tgt.append(k)
            if not f & 0x20:
                roll.append((t, f, _i32(-v["roll"])))
        if f & 0x200 and not f & 0x400:
            col.append((t, f, [v["r"], v["g"], v["b"]]))
        if f & 0x800 and not f & 0x1000:
            fov.append((t, f, _i32(_cdiv(v["fov"] << 16, 0x6487e))))
        t = _i32(t + v["dt"])
    n = len(pos)                       # ComputeTcbTangentsForKeys; key0 in = last out = 0
    if n == 2:
        d = _sub(pos[1].v, pos[0].v)
        pos[0].b = _scale(0x10000 - pos[0].tcb[0], d) + [0]
        pos[1].a = _scale(0x10000 - pos[1].tcb[0], d)
    elif n >= 3:
        for i in range(1, n - 1):
            _tcb_inner(pos[i - 1], pos[i + 1], pos[i])
        k0, k1, k2 = pos[0], pos[1], pos[2]
        k0.b = _tcb_end(_sub(k2.v, k0.v), _sub(k1.v, k0.v), k1.t - k0.t, k2.t - k0.t, k0.tcb[0]) + [0]
        kl, kp, kq = pos[-1], pos[-2], pos[-3]
        kl.a = _tcb_end(_sub(kl.v, kq.v), _sub(kl.v, kp.v), kl.t - kp.t, kl.t - kq.t, kl.tcb[0])
    run = [0x10000, 0, 0, 0]           # BuildRotationKeyQuats: flag 2, not 4/0x20
    for k in rot:
        if k.f & 2 and not k.f & 0x24:
            q = _axis_angle_quat(k.v[0], *k.a)
            if not k.f & 0x2000:
                q = _qmul_fixed(q, run)                     # run (x) q_rel
            run = q
            k.b = list(q)
        k.f |= 4
    return {"pos": pos, "rot": rot, "target": tgt, "roll": roll, "colour": col, "fov": fov}


def _interp_pos(keys, i, t):
    """Scene::InterpolatePositionKey 0x0048d0c0, segment i -> i+1: Hermite when key i has
    0x4000 (parameter = TcbEaseCurve(s, ease out of i, ease in of i+1)), else linear."""
    k0, k1 = keys[i], keys[i + 1]
    s = _fixdiv(t - k0.t, k1.t - k0.t)
    if k0.f & 0x4000:
        e = _ease(s, k0.tcb[4], k1.tcb[3])
        e2 = _r16(e, e)
        e3 = _r16(e2, e)
        h00 = e3 * 2 + 0x10000 - e2 * 3
        h01 = e2 * 3 - e3 * 2
        h10 = e3 - e2 * 2 + e
        h11 = e3 - e2
        r = _add(_scale(h01, k1.v), _scale(h00, k0.v))
        r = _add(_scale(h10, k0.b[:3]), r)
        return _add(_scale(h11, k1.a), r)
    return _add(_scale(s, k1.v), _scale(0x10000 - s, k0.v))


def _log_key(k):
    """(0, axis * half angle) of a rotation key's own axis-angle, as InterpolateRotationKeyTcb
    makes it: theta = angle/65536 * float(pi); theta >= float(pi)/2 (i.e. angle >= 0x8000)
    -> theta - float(pi); m = trunc(theta * 65536); xyz = round16(axis_stored * m)."""
    a = k.v[0]
    if a >= 0x8000:
        a -= 0x10000
    m = _cdiv(a * _PI_F_NUM, 1 << 22)
    return [0, _r16(k.a[0], m), _r16(k.a[1], m), _r16(k.a[2], m)]


def _qexp(g):
    """exp of a 16.16 log vector, as InterpolateRotationKeyTcb does it: m = trunc(sqrt(float(|g|^2)))
    w = trunc(cos(m/65536)*65536); |float sin| < 0.001 -> xyz = g, else xyz = g*trunc(sin*65536)/m."""
    import math
    m = int(math.sqrt(_f32(float(g[1]) * g[1] + float(g[2]) * g[2] + float(g[3]) * g[3])))
    th = m * 1.52587890625e-05
    sn = _f32(math.sin(th))
    w = int(math.cos(th) * 65536.0)
    if abs(sn) < 0.001:
        return [w, g[1], g[2], g[3]]
    if m == 0:
        return [w, 0, 0, 0]
    si = int(sn * 65536.0)
    return [w] + [_i32(_cdiv(c * si, m)) for c in g[1:]]


def _interp_rot(keys, i, t):
    """Scene::InterpolateRotationKeyTcb 0x0048d2a0, segment i -> i+1: plain slerp(q_i, q_i+1, s)
    unless key i or i+1 has 0x8000; then squad slerp(slerp(q_i, q_i+1, e), slerp(a_i, b_i+1, e),
    2e(1-e)), e = TcbEaseCurve(s, ease out of i, ease in of i+1)."""
    n = len(keys)
    k0, k1 = keys[i], keys[i + 1]
    t = min(max(t, 0), k1.t)
    s = _fixdiv(t - k0.t, k1.t - k0.t)
    if not k0.f & 0x8000 and not k1.f & 0x8000:
        return _slerp(k0.b, k1.b, s)
    L1 = _log_key(k1)                                     # log(q_i^-1 q_i+1) for relative keys
    if i < 1:
        L0, adj0 = list(L1), 0x10000
    else:
        L0 = _log_key(k0)
        dd = k1.t - keys[i - 1].t
        r = 0 if dd == 0 else _i32(_cdiv((k0.t - keys[i - 1].t) * 0x20000, dd))
        C = k0.tcb[1]
        adj0 = C - _r16(0x20000 - r, C) + (0x20000 - r)   # (1-C)(2-r) + C
    if i + 1 < n - 1:
        L2 = _log_key(keys[i + 2])
        dd = keys[i + 2].t - k0.t
        r = 0 if dd == 0 else _i32(_cdiv((k1.t - k0.t) * 0x20000, dd))
        C = k1.tcb[1]
        adj1 = C - _r16(r, C) + r                         # (1-C) r + C
    else:
        L2, adj1 = list(L1), 0x10000
    T, C, B = k0.tcb[0], k0.tcb[1], k0.tcb[2]
    wa = _r16(_cdiv(_r16(_r16(0x10000 - T, C + 0x10000), B + 0x10000), 2), adj0)
    wb = _r16(_cdiv(_r16(_r16(0x10000 - T, 0x10000 - C), 0x10000 - B), 2), adj0)
    g = [_cdiv(_r16(wb - 0x10000, L1[j]) + _r16(wa, L0[j]), 2) for j in range(4)]
    qa = _qmul_fixed(_qexp(g), k0.b)                      # a_i = q_i (x) exp(g)
    T, C, B = k1.tcb[0], k1.tcb[1], k1.tcb[2]
    wa = _r16(_cdiv(_r16(_r16(0x10000 - T, 0x10000 - C), B + 0x10000), 2), adj1)
    wb = _r16(_cdiv(_r16(_r16(0x10000 - T, C + 0x10000), 0x10000 - B), 2), adj1)
    h = [_cdiv(_r16(-wb, L2[j]) + _r16(0x10000 - wa, L1[j]), 2) for j in range(4)]
    qb = _qmul_fixed(_qexp(h), k1.b)                      # b_i+1 = q_i+1 (x) exp(h)
    e = _ease(s, k0.tcb[4], k1.tcb[3])
    p = _slerp(k0.b, k1.b, e)
    r = _slerp(qa, qb, e)
    return _slerp(p, r, _r16(e, 0x10000 - e) * 2)


def _atan16(y, x):
    """The octant code of AnimateNode (yaw and pitch): the angle of (x, y) from +x towards +y in
    16-bit turns (0..0xffff), via Math::g_AtanTable[FixedDivide(small, big)]; (0, 0) -> 0."""
    A = _tables()[1]
    if y < 0:
        a = -y
        if x < 0:
            if x < y:
                return A[_fixdiv(a, -x)] + 0x8000
            return 0xc000 - A[_fixdiv(-x, a)]
        if a < x:
            return -A[_fixdiv(a, x)] & 0xffff
        return A[_fixdiv(x, a)] + 0xc000
    if x < 0:
        b = -x
        if y < b:
            return 0x8000 - A[_fixdiv(y, b)]
        return A[_fixdiv(b, y)] + 0x4000
    if y < x:
        return A[_fixdiv(y, x)]
    if x == 0 and y == 0:
        return 0
    return 0x4000 - A[_fixdiv(x, y)]


def _track_value(keys, t, lerp):
    """(time, flags, value) track of 12/20-byte keys: the key value at a key time or past the
    last key, else lerp(t - t0, t1 - t0, v1, v0)."""
    i = _find_key([k[0] for k in keys], t)
    t0, _, v0 = keys[i]
    if t == t0 or i == len(keys) - 1:
        return i, v0, None
    return i, v0, lerp(t - t0, keys[i + 1][0] - t0, keys[i + 1][2], v0)


def evaluate_tracks(part, t_ms, tracks=None):
    """Everything Scene::AnimateNode computes for one part at time t (ms from the part's start,
    clamped to >= 0; past the last key a track holds its last value - looping is the caller's
    job). Fixed-point results:
      pos    [x, y, z] ints, engine units, parent space (None without position keys; a CONS/K
             constraint would replace it by another node's world position - not modelled)
      matrix 3x3 rows in 16.16 (node+0x40), None without rotation/target/roll tracks
      angles node+0x200..+0x20c afterwards (yaw, pitch, roll, - or a quaternion w, x, y, z)
      fov    turns in 16.16 (radians = fov/65536*2pi) or None; colour [r, g, b] 0..255 or None
    The rotation state node+0x200.. is taken as zero before the call (the engine keeps it from
    the previous frame; it matters only to a part with target keys but no roll track that also
    has rotation keys, which no shipped scene has)."""
    tr = tracks or engine_tracks(part)
    t = max(int(t_ms), 0)
    out = {"pos": None, "matrix": None, "fov": None, "colour": None}
    st = [0, 0, 0, 0]
    keys = tr["pos"]
    if keys:
        i = _find_key([k.t for k in keys], t)
        if t == keys[i].t or i == len(keys) - 1:
            out["pos"] = list(keys[i].v)
        else:
            out["pos"] = _interp_pos(keys, i, t)
    p = out["pos"] or [0, 0, 0]
    keys = tr["rot"]
    if keys:
        i = _find_key([k.t for k in keys], t)
        k = keys[i]
        if t == k.t or i == len(keys) - 1:
            if not k.f & 2:
                st = [k.v[0] & 0xffff, k.v[1] & 0xffff, k.v[2] & 0xffff, st[3]]
                out["matrix"] = _angles_matrix(*st[:3])
            else:
                st = list(k.b)
                out["matrix"] = _quat_matrix(st)
        elif not k.f & 2 and not keys[i + 1].f & 2:       # three angles: raw LerpInt, no wrap
            k1 = keys[i + 1]
            st = [_lerp_int(t - k.t, k1.t - k.t, k1.v[j], k.v[j]) & 0xffff for j in range(3)] + [st[3]]
            out["matrix"] = _angles_matrix(*st[:3])
        else:
            st = _interp_rot(keys, i, t)
            out["matrix"] = _quat_matrix(st)
    keys = tr["target"]
    if keys:
        i = _find_key([k.t for k in keys], t)
        if t == keys[i].t or i == len(keys) - 1:
            g = keys[i].v
        else:
            g = _interp_pos(keys, i, t)
        u, v = p[0] - g[0], g[2] - p[2]
        yaw = _atan16(u, v)                                # atan2(px - tx, tz - pz)
        hor = _r16(v, _cos16(yaw)) + _r16(u, _sin16(yaw))  # horizontal distance
        pitch = _atan16(g[1] - p[1], hor)                  # atan2(ty - py, hor)
        st = [yaw & 0xffff, pitch & 0xffff, st[2], st[3]]
        out["matrix"] = _angles_matrix(st[0], st[1], st[2])
    keys = tr["roll"]
    if keys:
        def roll_lerp(dt, dur, r1, r0):
            d = (r1 - r0) & 0xffff                         # shortest 16-bit arc
            if d > 0x8000:
                d -= 0x10000
            return _lerp_int(dt, dur, d + r0, r0)
        i, r0, r = _track_value(keys, t, roll_lerp)
        st[2] = r0 & 0xffff if r is None else r
        out["matrix"] = _angles_matrix(st[0], st[1], st[2])
    keys = tr["colour"]
    if keys:
        i, c0, c = _track_value(keys, t, lambda dt, dur, b, a: [_lerp_int(dt, dur, b[j], a[j])
                                                               for j in range(3)])
        out["colour"] = [((x * 0xff + 0x8000) >> 16) & 0xffff for x in (c0 if c is None else c)]
    keys = tr["fov"]
    if keys:
        i, f0, f = _track_value(keys, t, _lerp_int)
        out["fov"] = f0 if f is None else f
    out["angles"] = st
    return out


def evaluate(part, t_ms, tracks=None):
    """(position (x, y, z) floats in engine units, parent space; 3x3 rotation matrix as the
    engine's rows, floats; v_parent = v_local * M + position for row vectors) at time t_ms.
    Identity when the part has no rotation, target or roll track. Pass
    tracks=engine_tracks(part) when calling repeatedly."""
    r = evaluate_tracks(part, t_ms, tracks)
    p = r["pos"] or [0, 0, 0]
    m = r["matrix"] or [[0x10000, 0, 0], [0, 0x10000, 0], [0, 0, 0x10000]]
    return (tuple(float(c) for c in p), [[c / 65536.0 for c in row] for row in m])


def animation_length(part, tracks=None):
    """Largest last-key time over the part's engine tracks (as Scene::GetAnimationLength 0x0048cec0)."""
    tr = tracks or engine_tracks(part)
    ends = [(v[-1].t if isinstance(v[-1], _K) else v[-1][0]) for v in tr.values() if v]
    return max([0] + ends)


def sample(part, step_ms):
    """[(t, position, matrix)] from 0 to animation_length(part) every step_ms, the end included."""
    tr = engine_tracks(part)
    end = animation_length(part, tr)
    out, t = [], 0
    while True:
        out.append((t,) + evaluate(part, t, tr))
        if t >= end:
            break
        t = min(t + step_ms, end)
    return out


# --------------------------------------------------------------------------------------------
# Catalogue access for the acceptance run (.cat: every byte XOR (0xDB + i); .dat: XOR 0x33; see docs/formats.md)


def _read_cat(path):
    raw = open(path, "rb").read()
    raw = bytes(b ^ ((0xDB + i) & 0xFF) for i, b in enumerate(raw))
    lines = raw.decode("latin-1").splitlines()
    ent, off = [], 0
    for ln in lines[1:]:
        if not ln.strip():
            continue
        name, size = ln.rsplit(" ", 1)
        ent.append((name.replace("\\", "/"), off, int(size)))
        off += int(size)
    return ent


T33 = bytes(b ^ 0x33 for b in range(256))


def _decode(d):
    d = d.translate(T33)
    if d[:2] == b"\x1f\x8b":
        return gzip.decompress(d)
    if len(d) > 3:
        k = d[0] ^ 0xC8
        if d[1] ^ k == 0x1F and d[2] ^ k == 0x8B:
            return gzip.decompress(d[1:].translate(bytes(b ^ k for b in range(256))))
    return d


def iter_members(game, exts, addon_max=None):
    """Every member (not only the winners) of NN.cat then addon/NN.cat, mount order of
    Init::EngineConfigInit (stops at the first missing number). addon_max limits addon slots."""
    for sub in ("", "addon"):
        i = 1
        while True:
            if sub and addon_max is not None and i > addon_max:
                break
            cat = os.path.join(game, sub, "%02d.cat" % i)
            if not os.path.exists(cat):
                break
            dat = open(cat[:-4] + ".dat", "rb").read()
            layer = (sub + "/" if sub else "") + "%02d" % i
            for name, off, size in _read_cat(cat):
                if name.lower().endswith(exts):
                    yield layer, name, _decode(dat[off:off + size])
            i += 1


def check(game, addon_max=4, verbose=False):
    cut, text, other = {}, {}, 0
    cut_ok = cut_bad = 0
    txt_ok = txt_bad = 0
    txt_rt_same = txt_rt_exact = txt_rt_norm = txt_lenient = 0
    t2c_same = inline_files = inline_parts = 0
    bad = []
    for layer, name, d in iter_members(game, (".pbb", ".bob", ".pbd", ".bod"), addon_max):
        key = (layer, name)
        if d[:4] == b"CUT1":
            try:
                sc = read_cut1(d)
                w = write_cut1(sc)
                if w == d:
                    cut_ok += 1
                else:
                    cut_bad += 1
                    bad.append("CUT1 mismatch %s:%s" % key)
                cut[key] = sc
            except FormatError as e:
                cut_bad += 1
                bad.append("CUT1 %s:%s: %s" % (layer, name, e))
            continue
        if b"VER:" not in d or d[:4] == b"BOB1":
            other += 1
            continue
        # a text scene: the first token after comments is VER:
        r = _TR(d)
        r.skip_ws()
        if d[r.p:r.p + 4] != b"VER:":
            other += 1
            continue
        try:
            sc = read_text(d)
            txt_ok += 1
        except FormatError as e:
            txt_bad += 1
            bad.append("TEXT (engine parser fails too) %s:%s: %s" % (layer, name, e))
            try:
                sc = read_text(d, strict=False)
                txt_lenient += 1
            except FormatError as e2:
                bad.append("TEXT lenient %s:%s: %s" % (layer, name, e2))
                continue
        text[key] = sc
        w = write_text(sc)
        sc2 = read_text(w, strict=False)
        v, _ = compare(sc, sc2, tol=0)
        if v == "same":
            txt_rt_same += 1
        else:
            bad.append("text->text %s:%s %s" % (layer, name, v))
        if w == d:
            txt_rt_exact += 1
        if _strip_text(w) == _strip_text(d):
            txt_rt_norm += 1
        elif verbose:
            bad.append("text not normal-equal %s:%s" % key)
        # text -> CUT1 -> tree must be the same tree too (inline text bodies are not carried over)
        c2 = read_cut1(write_cut1(sc))
        v2, det2 = compare(sc, c2, tol=0, embedded=False)
        if v2 == "same":
            t2c_same += 1
        else:
            bad.append("text->CUT1 %s:%s %s %s" % (layer, name, v2, det2[:2]))
        n_inline = sum(1 for p in sc.parts if p.inline is not None)
        if n_inline:
            inline_files += 1
            inline_parts += n_inline
    # CUT1 -> text -> tree (only scenes without embedded bodies: text cannot hold a BOB1)
    c2t = c2t_same = 0
    for key, sc in cut.items():
        if any(p.embedded is not None for p in sc.parts):
            continue
        c2t += 1
        v, det = compare(sc, read_text(write_text(sc)), tol=0)
        if v == "same":
            c2t_same += 1
        else:
            bad.append("CUT1->text %s:%s %s %s" % (key[0], key[1], v, det[:2]))
    print("CUT1 members: %d, byte-exact round trip: %d, failed: %d" % (cut_ok + cut_bad, cut_ok, cut_bad))
    print("CUT1 -> text -> tree, scenes without embedded bodies: same tree %d/%d" % (c2t_same, c2t))
    print("text scenes: %d, parsed as the engine does: %d, rejected by the engine's parser too: %d"
          " (of these read in lenient mode: %d)" % (txt_ok + txt_bad, txt_ok, txt_bad, txt_lenient))
    print("text -> write_text -> read: same tree %d/%d; byte-identical %d; identical after"
          " dropping comments and whitespace %d" % (txt_rt_same, len(text), txt_rt_exact, txt_rt_norm))
    print("text -> CUT1 -> tree: same tree (inline bodies aside) %d/%d; files with inline 'L'"
          " bodies %d (%d parts), whose text bodies a CUT1 writer would have to convert to BOB1"
          % (t2c_same, len(text), inline_files, inline_parts))
    print("other members (bodies): %d" % other)
    # stems with both forms
    def stem(n):
        return os.path.splitext(n.lower())[0]
    cs = {}
    for (layer, n), sc in cut.items():
        cs.setdefault(stem(n), []).append((layer, n, sc))
    both = 0
    verdicts = {}
    for (layer, n), sc in sorted(text.items()):
        s = stem(n)
        if s not in cs:
            continue
        for cl, cn, csc in cs[s]:
            both += 1
            v, det = compare(csc, sc)
            verdicts[v] = verdicts.get(v, 0) + 1
            print("  both forms: %s:%s vs %s:%s -> %s%s" % (cl, cn, layer, n, v,
                  (" (" + "; ".join(det[:3]) + (" ..." if len(det) > 3 else "") + ")") if det else ""))
    print("stem pairs with both CUT1 and text: %d %r" % (both, verdicts))
    for b in bad[:40]:
        print("  " + b)
    return cut_bad == 0 and txt_bad == 0


def check_interp(game, addon_max=4):
    """Self-check of evaluate() over every installed scene part with TCB (0x4000/0x8000) or target
    (8) keys: at key times the position is the key value, the rotation matches rotation_quats()
    (float accumulation; the engine's fixed-point drift is reported), matrices are orthonormal, a
    target key's row 2 points at the target; AnglesToMatrix == QuatToMatrixRows(qz(-c) qx(b) qy(a))."""
    import math
    import random

    def qa(axis, phi):
        s = math.sin(phi / 2)
        return (math.cos(phi / 2),) + tuple(a * s for a in axis)

    def qm(q):
        w, x, y, z = q
        return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]

    def diff(m, f):
        return max(abs(m[i][j] / 65536.0 - f[i][j]) for i in range(3) for j in range(3))

    rnd = random.Random(1)
    k = 2 * math.pi / 65536
    worst = 0.0
    for _ in range(2000):
        a, b, c = rnd.randrange(65536), rnd.randrange(65536), rnd.randrange(65536)
        q = _qmul(_qmul(qa((0, 0, 1), -c * k), qa((1, 0, 0), b * k)), qa((0, 1, 0), a * k))
        worst = max(worst, diff(_angles_matrix(a, b, c), qm(q)))
    print("AnglesToMatrix vs QuatToMatrixRows(qz(-c) qx(b) qy(a)): max %.2g" % worst)
    n = {"parts": 0, "pos": 0, "pos_bad": 0, "rot": 0, "tgt": 0}
    mx = {"rot": 0.0, "ortho": 0.0, "tgt_deg": 0.0}
    for layer, name, data in iter_members(game, (".pbb", ".bob", ".pbd", ".bod"), addon_max):
        try:
            sc = read(data, strict=False)
        except FormatError:
            continue
        for p in sc.parts:
            if not any(kk.flags & 0xc008 for kk in p.keys):
                continue
            n["parts"] += 1
            tr = engine_tracks(p)
            times = key_times(p)
            quats = [q for q in rotation_quats(p) if q is not None]
            for kk in tr["pos"]:
                r = evaluate_tracks(p, kk.t, tr)
                n["pos"] += 1
                j = _find_key([x.t for x in tr["pos"]], kk.t)
                n["pos_bad"] += r["pos"] != tr["pos"][j].v
            rk = tr["rot"]
            if len(quats) == len(rk) and not tr["target"]:
                for j, kk in enumerate(rk):
                    if _find_key([x.t for x in rk], kk.t) != j:
                        continue                             # several keys at one time
                    r = evaluate_tracks(p, kk.t, tr)
                    n["rot"] += 1
                    mx["rot"] = max(mx["rot"], diff(r["matrix"], qm(quats[j])))
            end = animation_length(p, tr)
            for t in range(0, end + 1, max(1, end // 50)):
                m = evaluate_tracks(p, t, tr)["matrix"]
                if m:
                    e = max(abs(sum(m[a][x] * m[b][x] for x in range(3)) / 2.0 ** 32 - (a == b))
                            for a in range(3) for b in range(3))
                    mx["ortho"] = max(mx["ortho"], e)
            for kk in tr["target"]:
                r = evaluate_tracks(p, kk.t, tr)
                d = [g - q for g, q in zip(kk.v, r["pos"] or [0, 0, 0])]
                ln = math.sqrt(sum(c * c for c in d))
                if ln < 1:
                    continue
                n["tgt"] += 1
                cs = sum(r["matrix"][2][x] / 65536.0 * d[x] / ln for x in range(3))
                mx["tgt_deg"] = max(mx["tgt_deg"], math.degrees(math.acos(max(-1.0, min(1.0, cs)))))
    print("parts %(parts)d; position keys %(pos)d, not the key value %(pos_bad)d; rotation keys %(rot)d;"
          " target keys %(tgt)d" % n)
    print("rotation at keys vs rotation_quats: max %.4g (fixed-point accumulation); orthonormality max"
          " %.3g; target direction max %.3g deg" % (mx["rot"], mx["ortho"], mx["tgt_deg"]))
    return n["pos_bad"] == 0 and mx["rot"] < 0.02 and mx["ortho"] < 0.002 and mx["tgt_deg"] < 1


def _strip_text(b):
    """Text without comments and whitespace (the engine ignores both)."""
    out = []
    for ln in b.decode("latin-1").splitlines():
        i = ln.find("/")
        if i >= 0:
            ln = ln[:i]
        out.append("".join(ln.split()))
    return "".join(out)


def dump(sc, f=sys.stdout):
    print("form %s version %s info %r parts %d" % (sc.form, sc.version, sc.info, len(sc.parts)), file=f)
    for p in sc.parts:
        print("P %d body %r C %d F %d flags 0x%x name %r keys %d%s%s%s" % (
            p.index, p.body, p.mode, p.parent, p.flags, p.name, len(p.keys),
            " K%r" % p.constraints if p.constraints is not None else "",
            " T%r" % p.notes if p.notes else "",
            " embedded %d bytes" % len(p.embedded) if p.embedded else
            (" inline %d chars" % len(p.inline[1]) if p.inline else "")), file=f)
        for k in p.keys:
            print("   0x%04x %s" % (k.flags, " ".join("%s=%d" % kv for kv in k.values.items())), file=f)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "check":
        addon = 4
        if "--addon" in argv:
            addon = int(argv[argv.index("--addon") + 1])
        return 0 if check(argv[2], addon, "-v" in argv) else 1
    if cmd == "interp":
        return 0 if check_interp(argv[2]) else 1
    data = open(argv[2], "rb").read()
    sc = read(data)
    if cmd == "dump":
        dump(sc)
    elif cmd == "tocut1":
        open(argv[3], "wb").write(write_cut1(sc))
    elif cmd == "totext":
        open(argv[3], "wb").write(write_text(sc))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

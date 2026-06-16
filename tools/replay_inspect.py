"""
Inspect a Rocket League .replay file's header and dump the values SyncComms
uses for audio sync anchoring: NumFrames and Goals[i].frame.

Mirrors src/standalone/ReplayMetadata.cpp, stdlib only.

Usage:
    py tools/replay_inspect.py "<path-to-.replay>"

Output: one block per file showing
    - MatchGuid
    - NumFrames + total length in seconds (assuming 30 fps)
    - Each goal as (index, frame, frame/30 = seconds-into-replay)

Seconds are at 30 fps (Frame is wall-clock-30fps in the replay file —
verified empirically from the probe CSVs). Compare these against the
visual goal moment in RL's replay viewer to determine whether `goalFrames`
points to ball-in-net, cinematic-start, or somewhere else.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def need(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise EOFError(f"want {n} bytes at {self.pos}, only {len(self.data) - self.pos} left")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u32(self) -> int: return struct.unpack("<I", self.need(4))[0]
    def i32(self) -> int: return struct.unpack("<i", self.need(4))[0]
    def u64(self) -> int: return struct.unpack("<Q", self.need(8))[0]
    def u8(self)  -> int: return self.need(1)[0]
    def f32(self) -> float: return struct.unpack("<f", self.need(4))[0]
    def f64(self) -> float: return struct.unpack("<d", self.need(8))[0]
    def skip(self, n: int) -> None: self.pos += n

    def string(self) -> str:
        ln = self.i32()
        if ln == 0:
            return ""
        if ln > 0:
            buf = self.need(ln)
            return buf.rstrip(b"\x00").decode("latin-1", errors="replace")
        chars = -ln
        buf = self.need(chars * 2)
        # Drop trailing null pair, decode UTF-16 LE
        if chars > 0:
            buf = buf[:-2]
        return buf.decode("utf-16-le", errors="replace")


def parse_value(r: Reader, ptype: str, psize: int):
    if ptype == "IntProperty":
        return r.i32()
    if ptype == "FloatProperty":
        return r.f64() if psize == 8 else r.f32()
    if ptype == "BoolProperty":
        # RL .replay BoolProperty: the value is the size field itself (size=1
        # → true, size=0 → false). NO additional byte follows. The C++
        # parser's r.U8(v) read consumes one byte too many and desyncs the
        # next property name read; that's the well-known "MatchGuid /
        # NumFrames never extracted" symptom.
        return psize != 0
    if ptype in ("StrProperty", "NameProperty"):
        return r.string()
    if ptype == "QWordProperty":
        return r.u64()
    if ptype == "ByteProperty":
        # RL/UE3 layout: (enum_name string) (1 byte index) (enum_value string)
        # The C++ parser is missing the index byte and desyncs after this.
        enum_name = r.string()
        _idx = r.u8()
        enum_value = r.string()
        return enum_value
    if ptype == "ArrayProperty":
        count = r.u32()
        if count > 4096:
            raise ValueError(f"array too large: {count}")
        return [parse_dict(r) for _ in range(count)]
    # Unknown — best-effort skip
    r.skip(psize)
    return None


def parse_dict(r: Reader) -> dict:
    """Parse a property dict. Tolerates trailing garbage by returning whatever
    was successfully parsed before the first failure — matches the C++ parser's
    de facto behavior (it bails on unknown struct-typed properties via a bad
    size skip, but the caller still uses out.numFrames / out.goalFrames if
    they were read earlier in the dict)."""
    d: dict = {}
    for _ in range(256):
        start_pos = r.pos
        try:
            name = r.string()
        except (EOFError, UnicodeDecodeError, ValueError, struct.error) as e:
            d["__parse_stopped__"] = ("info",
                f"reading next name at byte {start_pos}: {e.__class__.__name__}: {e}; "
                f"raw next 16 bytes = {r.data[start_pos:start_pos+16].hex(' ')}")
            return d
        if name == "None":
            return d
        ptype = "<unread>"
        try:
            ptype = r.string()
            psize = r.u64()
            d[name] = (ptype, parse_value(r, ptype, psize))
        except (EOFError, UnicodeDecodeError, ValueError, struct.error) as e:
            d["__parse_stopped__"] = ("info",
                f"at property name={name!r} type={ptype!r}: {e.__class__.__name__}: {e}")
            return d
    return d


def parse_replay(path: Path) -> dict:
    data = path.read_bytes()
    r = Reader(data)
    header_size = r.u32()
    header_crc = r.u32()
    engine_v = r.u32()
    licensee_v = r.u32()
    net_v = 0
    if engine_v > 868 or (engine_v == 868 and licensee_v >= 18):
        net_v = r.u32()
    class_name = r.string()
    props = parse_dict(r)
    return {
        "engine": engine_v, "licensee": licensee_v, "net": net_v,
        "class": class_name, "props": props,
        "stopped_at": r.pos, "file_size": len(data),
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    fps = 30.0
    for arg in argv[1:]:
        path = Path(arg)
        if not path.exists():
            print(f"!! not found: {path}", file=sys.stderr)
            continue
        try:
            r = parse_replay(path)
        except Exception as e:
            print(f"!! parse failed for {path.name}: {e}", file=sys.stderr)
            continue

        props = r["props"]
        guid = props.get("MatchGuid", (None, ""))[1]
        num_frames = props.get("NumFrames", (None, 0))[1] or 0
        goals = props.get("Goals", (None, []))[1] or []

        print(f"=== {path.name} ===")
        print(f"  engine/licensee/net: {r['engine']}/{r['licensee']}/{r['net']}")
        print(f"  parsed bytes       : {r['stopped_at']} / {r['file_size']}"
              + ("" if r['props'] else "  (parse stopped immediately — file may be malformed)"))
        print(f"  property keys      : {sorted(props.keys())}")
        for k in sorted(props.keys()):
            t, v = props[k]
            if isinstance(v, list):
                v = f"[{len(v)} items]"
            print(f"    {k:<28} ({t}) = {v}")
        print(f"  MatchGuid          : {guid}")
        print(f"  NumFrames          : {num_frames}  ({num_frames/fps:.2f}s @ 30fps)")
        print(f"  Goals (n={len(goals)}):")
        prev_sec = 0.0
        for i, g in enumerate(goals):
            entry = g.get("frame") or g.get("Frame") or (None, None)
            frame = entry[1] if entry else None
            if frame is None:
                print(f"    [{i}]  (no frame field — keys: {list(g.keys())})")
                continue
            sec = frame / fps
            scorer_team = (g.get("PlayerTeam") or (None, None))[1]
            print(f"    [{i}]  frame={frame:>5}  sec={sec:7.2f}  delta_from_prev={sec - prev_sec:+6.2f}s"
                  + (f"  team={scorer_team}" if scorer_team is not None else ""))
            prev_sec = sec
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Check four connected extrusion layers above DynaPin landings (offline only).

Landing fan markers seed the check. Extruded lines are capsules of radius
WIDTH/2; touching capsules at one Z form a component. Components propagate
upward only when the upper extrusion's bottom touches the lower top in Z.
Travel, pin actuation and disconnected material are excluded. This reconstructs
connectivity from G-code, not from the slicer's original support polygons.
Files with pulls but no landing markers are UNKNOWN, never silently PASS.
An independent whole-file check rejects XYZ moves at X >= 168 outside
BEGIN_DYNAPIN_PULL / END_DYNAPIN_PULL, including travel and return moves.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import sys


NUMBER = r"[-+]?(?:\d+\.?\d*|\.\d+)"
WORDS = re.compile(r"([A-Z])(" + NUMBER + r")")
PULL = re.compile(r";\s*BEGIN_DYNAPIN_PULL ROW=(\d+) COL=(\d+)")
TOL = 0.002  # mm: accommodate rounded G-code XYZ and HEIGHT comments
EXCLUDED_ROLES = {"Custom", "Skirt", "Brim", "Wipe tower", "Prime tower", "Travel"}


@dataclass(frozen=True, slots=True)
class Segment:
    line: int
    a: tuple[float, float]
    b: tuple[float, float]
    z: float
    height: float
    width: float
    role: str
    landing: bool = False

    @property
    def bounds(self):
        r = self.width / 2
        return (min(self.a[0], self.b[0]) - r, min(self.a[1], self.b[1]) - r,
                max(self.a[0], self.b[0]) + r, max(self.a[1], self.b[1]) + r)


def point_distance2(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    length2 = dx * dx + dy * dy
    t = max(0., min(1., ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length2)) if length2 else 0.
    return (p[0] - a[0] - t * dx) ** 2 + (p[1] - a[1] - t * dy) ** 2


def touches(s, t):
    def cross(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])

    # Strict crossings; endpoint and collinear cases use point distances.
    crossing = (cross(s.a, s.b, t.a) * cross(s.a, s.b, t.b) < 0 and
                cross(t.a, t.b, s.a) * cross(t.a, t.b, s.b) < 0)
    distance2 = 0. if crossing else min(point_distance2(s.a, t.a, t.b), point_distance2(s.b, t.a, t.b),
                                      point_distance2(t.a, s.a, s.b), point_distance2(t.b, s.a, s.b))
    return distance2 <= ((s.width + t.width) / 2 + TOL) ** 2


class SpatialIndex:
    """Small uniform grid for capsule queries; avoids all-pairs comparisons."""

    def __init__(self):
        self.cells = defaultdict(list)

    @staticmethod
    def keys(segment):
        x0, y0, x1, y1 = segment.bounds
        for x in range(math.floor((x0 - TOL) / 4), math.floor((x1 + TOL) / 4) + 1):
            for y in range(math.floor((y0 - TOL) / 4), math.floor((y1 + TOL) / 4) + 1):
                yield x, y

    def add(self, i, segment):
        for key in self.keys(segment):
            self.cells[key].append(i)

    def query(self, segment):
        return {i for key in self.keys(segment) for i in self.cells.get(key, ())}


def discover(root):
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() == ".gcode"
                  and p.stem.lower() not in {"min", "py"}
                  and not p.name.lower().startswith(("min_", "py_")))


def parse_gcode(path, layers):
    max_height = 0.
    has_pulls = False
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for raw in source:
            digest.update(raw)
            has_pulls |= PULL.match(raw.decode("utf-8", errors="replace").strip()) is not None
            if raw.startswith(b";HEIGHT:"):
                max_height = max(max_height, float(raw.split(b":", 1)[1]))
    if not has_pulls:
        return [], [], digest.hexdigest()
    if max_height <= 0:
        raise ValueError("missing positive ;HEIGHT comments")

    xyz = [0., 0., 0.]
    e = 0.
    absolute_xyz, absolute_e = True, True
    width = height = 0.
    role = "Custom"
    landing = False
    in_motion_block = False
    trigger_z = None
    capture_until = -math.inf
    pulls, segments = [], []
    with path.open(encoding="utf-8", errors="replace") as source:
        for n, raw in enumerate(source, 1):
            line = raw.strip()
            if line.startswith(";Z:"):
                trigger_z = float(line[3:])
            elif line.startswith(";HEIGHT:"):
                height = float(line[8:])
            elif line.startswith(";WIDTH:"):
                width = float(line[7:])
            elif line.startswith(";TYPE:"):
                role = line[6:]
            elif line == ";_DYNAPIN_LANDING_FAN_START":
                landing = True
            elif line == ";_DYNAPIN_LANDING_FAN_END":
                landing = False
            match = PULL.match(line)
            if match:
                if trigger_z is None:
                    raise ValueError(f"line {n}: pull has no ;Z comment")
                pulls.append({"row": int(match[1]), "col": int(match[2]), "trigger_z": trigger_z, "line": n})
                # Any chain of <= layers extrusions must fit this height window.
                capture_until = max(capture_until, trigger_z + (layers + 1) * max_height + TOL)
            if line.startswith(("; BEGIN_DYNAPIN_PULL", "; BEGIN_DYNAPIN_RETURN")):
                in_motion_block = True
            elif line.startswith(("; END_DYNAPIN_PULL", "; END_DYNAPIN_RETURN")):
                in_motion_block = False
            command = line.split(";", 1)[0].strip()
            if not command:
                continue
            words = dict(WORDS.findall(command.upper()))
            op = command.split()[0].upper()
            if op in {"G20", "G28"}:
                if op == "G20":
                    raise ValueError(f"line {n}: inch units are unsupported")
                for axis in ("X", "Y", "Z"):
                    if len(command.split()) == 1 or axis in command.upper():
                        xyz["XYZ".index(axis)] = 0.
                continue
            if op in {"G90", "G91"}:
                absolute_xyz = absolute_e = op == "G90"
            elif op in {"M82", "M83"}:
                absolute_e = op == "M82"
            elif op == "G92":
                for i, axis in enumerate("XYZ"):
                    if axis in words:
                        xyz[i] = float(words[axis])
                if "E" in words:
                    e = float(words["E"])
            elif op in {"G0", "G1", "G2", "G3"}:
                start = tuple(xyz)
                for i, axis in enumerate("XYZ"):
                    if axis in words:
                        value = float(words[axis])
                        xyz[i] = value if absolute_xyz else xyz[i] + value
                new_e = float(words["E"]) if "E" in words else (e if absolute_e else 0.)
                delta_e = new_e - e if absolute_e else new_e
                e = new_e if absolute_e else e + new_e
                if delta_e <= 0 or (start[:2] == tuple(xyz[:2]) and op in {"G0", "G1"}):
                    continue
                if in_motion_block or role in EXCLUDED_ROLES or not pulls or xyz[2] > capture_until:
                    continue
                if op in {"G2", "G3"}:
                    raise ValueError(f"line {n}: extruding arcs are unsupported; linearize first")
                if abs(start[2] - xyz[2]) > TOL:
                    raise ValueError(f"line {n}: non-planar extrusion is unsupported")
                if width <= 0 or height <= 0:
                    raise ValueError(f"line {n}: missing positive WIDTH/HEIGHT")
                segments.append(Segment(n, start[:2], tuple(xyz[:2]), round(xyz[2], 6), height, width, role, landing))
    return segments, pulls, digest.hexdigest()


def analyze(segments, layers, x_min, x_max):
    """Return per-landing-Z/depth rows and individual offending source moves."""
    by_z = defaultdict(list)
    for s in segments:
        by_z[s.z].append(s)
    planes = {}
    for z, items in sorted(by_z.items()):
        parent = list(range(len(items)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        index = SpatialIndex()
        for i, s in enumerate(items):
            for j in index.query(s):
                if touches(s, items[j]):
                    parent[find(i)] = find(j)
            index.add(i, s)
        groups = defaultdict(list)
        for i in range(len(items)):
            groups[find(i)].append(i)
        planes[z] = (items, index, {i: find(i) for i in range(len(items))}, groups)

    seed_zs = sorted({s.z for s in segments if s.landing})
    rows, violations = [], []
    for seed_z in seed_zs:
        # A depth belongs to a connected component, never to unrelated global
        # LAYER_CHANGE events (normal/support heights can be interleaved).
        reached = {}
        items, _, roots, groups = planes[seed_z]
        for i, s in enumerate(items):
            if s.landing:
                reached[(seed_z, roots[i])] = 1
        for z, (items, index, roots, groups) in planes.items():
            if z < seed_z:
                continue
            predecessors = [(key, depth) for key, depth in reached.items() if key[0] < z and depth < layers]
            for (lower_z, lower_root), depth in predecessors:
                lower_items, _, _, lower_groups = planes[lower_z]
                for i in lower_groups[lower_root]:
                    s = lower_items[i]
                    for j in index.query(s):
                        t = items[j]
                        if abs(t.z - t.height - lower_z) <= TOL and touches(s, t):
                            key = (z, roots[j])
                            reached[key] = min(reached.get(key, layers + 1), depth + 1)
            for root, indices in groups.items():
                depth = reached.get((z, root))
                if depth is None or depth > layers:
                    continue
                selected = [items[i] for i in indices]
                bad = [s for s in selected if s.bounds[0] < x_min - 1e-6 or s.bounds[2] > x_max + 1e-6]
                rows.append({"landing_z": seed_z, "layer": depth, "z": z, "segments": len(selected),
                             "x_min": min(s.bounds[0] for s in selected), "x_max": max(s.bounds[2] for s in selected),
                             "roles": sorted({s.role for s in selected}), "violating_segments": len(bad)})
                for s in bad:
                    violations.append({"landing_z": seed_z, "layer": depth, "z": z, "line": s.line,
                                       "role": s.role, "x_start": s.a[0], "x_end": s.b[0], "width": s.width,
                                       "x_min": s.bounds[0], "x_max": s.bounds[2]})
    return rows, violations


def check_file(path, layers=4, x_min=60., x_max=165.):
    result = {"file": str(path), "status": "ERROR"}
    try:
        segments, pulls, sha256 = parse_gcode(path, layers)
        result.update(sha256=sha256, pulls=pulls)
        if not pulls:
            result.update(status="NO_PINS", reason="No DynaPin pull markers; not applicable")
            return result
        rows, violations = analyze(segments, layers, x_min, x_max)
        if not rows:
            result.update(status="UNKNOWN", reason="Pulls exist but no landing extrusion markers; cannot identify pin-connected material")
            return result
        cohorts = {r["landing_z"] for r in rows}
        incomplete = [z for z in sorted(cohorts) if max(r["layer"] for r in rows if r["landing_z"] == z) < layers]
        # Ensure every pull event has an identified landing nearby in height.
        unmatched = [p for p in pulls if not any(-TOL <= z - p["trigger_z"] <= max(s.height for s in segments) + TOL
                                                for z in cohorts)]
        result.update(status="FAIL" if violations else "UNKNOWN" if incomplete or unmatched else "PASS",
                      rows=rows, violations=violations, incomplete_landing_z=incomplete, unmatched_pulls=unmatched,
                      x_min=min(r["x_min"] for r in rows), x_max=max(r["x_max"] for r in rows))
        if incomplete or unmatched:
            result["reason"] = "Some landings have fewer than requested connected layers or pulls lack landing markers; see details"
    except (ValueError, OSError) as error:
        result["reason"] = str(error)
    return result


def check_motion_x(path, forbidden_x=168.):
    """Check nozzle-coordinate linear moves, independently of pins/landings.

    G28 X (or all axes) establishes X=0; firmware-generated homing motion is
    not visible in this file. G92 offsets are tracked in machine coordinates.
    A move leaving the forbidden region is also a violation if outside a pull.
    """
    result = {"status": "ERROR", "violations": [], "max_x": None}
    xyz = [None, None, None]
    offsets = [0., 0., 0.]
    absolute = True
    in_pull = False
    role = "Custom"
    try:
        with path.open(encoding="utf-8", errors="replace") as source:
            for n, raw in enumerate(source, 1):
                line = raw.strip()
                if PULL.match(line):
                    if in_pull:
                        raise ValueError(f"line {n}: nested pull block")
                    in_pull = True
                elif line == "; END_DYNAPIN_PULL":
                    if not in_pull:
                        raise ValueError(f"line {n}: unmatched pull end")
                    in_pull = False
                elif line.startswith(";TYPE:"):
                    role = line[6:]
                command = line.split(";", 1)[0].strip().upper()
                if not command:
                    continue
                words = dict(WORDS.findall(command))
                op = command.split()[0]
                if op == "G20":
                    raise ValueError(f"line {n}: inch units are unsupported")
                if op in {"G90", "G91"}:
                    absolute = op == "G90"
                elif op == "G28":
                    axes = [axis for axis in "XYZ" if axis in command]
                    for axis in axes or "XYZ":
                        i = "XYZ".index(axis)
                        xyz[i], offsets[i] = 0., 0.
                elif op == "G92":
                    for i, axis in enumerate("XYZ"):
                        if axis in words:
                            if xyz[i] is None:
                                raise ValueError(f"line {n}: G92 {axis} before known machine position")
                            offsets[i] = xyz[i] - float(words[axis])
                elif op in {"G0", "G1", "G2", "G3"}:
                    if op in {"G2", "G3"}:
                        raise ValueError(f"line {n}: arc bounds unsupported; linearize first")
                    if not any(axis in words for axis in "XYZ"):
                        continue
                    start = tuple(xyz)
                    for i, axis in enumerate("XYZ"):
                        if axis not in words:
                            continue
                        value = float(words[axis])
                        if absolute:
                            xyz[i] = value + offsets[i]
                        elif xyz[i] is not None:
                            xyz[i] += value
                        else:
                            raise ValueError(f"line {n}: relative {axis} move before known position")
                    if in_pull or tuple(xyz) == start:
                        continue
                    if xyz[0] is None:
                        raise ValueError(f"line {n}: XYZ move before known X position")
                    maximum = max(x for x in (start[0], xyz[0]) if x is not None)
                    result["max_x"] = maximum if result["max_x"] is None else max(result["max_x"], maximum)
                    if maximum >= forbidden_x:
                        result["violations"].append({"line": n, "x_start": start[0], "x_end": xyz[0],
                                                     "max_x": maximum, "role": role, "command": command})
            if in_pull:
                raise ValueError("unterminated pull block")
        result["status"] = "FAIL" if result["violations"] else "PASS"
    except (ValueError, OSError) as error:
        result["reason"] = str(error)
    return result


def write_markdown(path, report):
    low, high = report["x_limits_mm"]
    lines = ["# DynaPin ピン上の押出経路・X範囲検証", "",
             f"許容範囲: **X{low:g}〜X{high:g} mm（両端を含む）**。着地層を1層目として"
             f"{report['layers_including_landing']}層目までを対象とする。", "",
             "着地コメントを起点に、線幅の半分を半径とする線分領域の接続を追跡する。"
             "同じ高さの接続成分全体を含め、上層は押出高さから求めた下面が下層上面に接する場合だけ追跡する。"
             "トラベル、ピン引き出し動作、独立した造形領域は含めない。", "",
             "G-codeからの幾何学的な再構成であり、元のスライサポリゴンによる判定ではない。"
             "意図的なZギャップを越える支持関係や、着地コメントのない直接造形は確定できない。"
             "4層未満で追跡が途切れる、または引き出しに対応する着地を特定できない場合はUNKNOWNとする。", "",
             "`min.gcode` / `py.gcode` / `min_*` / `py_*` を除外。`old/`、`test/`、`mini/` は含む。", "",
             "集計: " + ", ".join(f"{k}={v}" for k, v in report["summary"].items()), "",
             "| ファイル | 結果 | 押出領域X最小 | 押出領域X最大 |", "|---|---|---:|---:|"]
    for result in report["files"]:
        source = Path(result["file"]).resolve()
        label = source.relative_to(Path(__file__).resolve().parents[1]) if source.is_relative_to(Path(__file__).resolve().parents[1]) else source
        bounds = f"{result['x_min']:.3f} | {result['x_max']:.3f}" if "x_min" in result else "— | —"
        lines.append(f"| [{label}](<{source}>) | {result['status']} | {bounds} |")
    lines.extend(["", "## 範囲外の詳細", ""])
    for result in report["files"]:
        if not result.get("violations"):
            continue
        worst = min(result["violations"], key=lambda v: min(v["x_min"] - low, high - v["x_max"]))
        source = Path(result["file"]).resolve()
        depths = sorted({v["layer"] for v in result["violations"]})
        lines.append(f"- [{source.name}](<{source}:{worst['line']}>)："
                     f"範囲外は第{', '.join(map(str, depths))}層。代表箇所は元ファイル{worst['line']}行目、"
                     f"Z={worst['z']:.6g}、{worst['role']}、線分中心X={worst['x_start']:.3f}→{worst['x_end']:.3f}、"
                     f"線幅={worst['width']:.3f} mm。")
    lines.extend(["", "## 判定不能の詳細", ""])
    for result in report["files"]:
        if result["status"] not in {"UNKNOWN", "ERROR"}:
            continue
        details = result.get("reason", "")
        if result.get("incomplete_landing_z"):
            details += f"; 追跡が4層に達しない着地Z={result['incomplete_landing_z']}"
        if result.get("unmatched_pulls"):
            details += "; 着地不明ピン=" + ", ".join(f"({p['row']},{p['col']}) Z={p['trigger_z']}" for p in result["unmatched_pulls"])
        lines.append(f"- {result['file']}: {details}")
    lines.extend(["", "全押出経路の範囲・層番号・違反箇所の行番号・入力SHA-256は同名JSONに保存。", ""])
    if "motion_forbidden_x_mm" in report:
        limit = report["motion_forbidden_x_mm"]
        lines.extend(["## 全移動の侵入検証", "",
                      f"全層・全XYZ直線移動について、ピン引き出しブロック外の **X{limit:g}以上** を禁止する。"
                      "押出なしのトラベル、開始・終了処理、DYNAPIN_RETURNも対象。境界値を含め、"
                      "線分の始点と終点を検査する。判定はノズル指令座標で、線幅やヘッド外形は加算しない。", "",
                      "G28はホーム位置X=0を設定する命令として扱う。ファームウェア内部のホーミング軌跡は検査できない。", "",
                      "侵入検証集計: " + ", ".join(f"{k}={v}" for k, v in report["motion_summary"].items()), "",
                      "| ファイル | 侵入検証 | 引き出し時以外の最大X | 違反移動数 |", "|---|---|---:|---:|"])
        for item in report["files"]:
            motion = item["motion"]
            source = Path(item["file"]).resolve()
            maximum = f"{motion['max_x']:.3f}" if motion["max_x"] is not None else "—"
            lines.append(f"| [{source.name}](<{source}>) | {motion['status']} | {maximum} | {len(motion['violations'])} |")
            if motion.get("reason"):
                lines.append(f"\n検査エラー: {motion['reason']}\n")
        for item in report["files"]:
            for move in item["motion"]["violations"][:5]:
                source = Path(item["file"]).resolve()
                lines.append(f"\n- [{source.name}:{move['line']}](<{source}:{move['line']}>) "
                             f"X={move['x_start']}→{move['x_end']}：`{move['command']}`")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main(argv=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, nargs="?", default=root / "models")
    parser.add_argument("--layers", type=int, default=4)
    parser.add_argument("--x-min", type=float, default=60.)
    parser.add_argument("--x-max", type=float, default=165.)
    parser.add_argument("--motion-forbidden-x", type=float, default=168.,
                        help="reject nozzle XYZ moves at or above this X outside pull blocks")
    parser.add_argument("--report", type=Path, default=root / "reports/dynapin_x_bounds.json")
    args = parser.parse_args(argv)
    if args.layers < 1 or not all(map(math.isfinite, (args.x_min, args.x_max, args.motion_forbidden_x))) or args.x_min > args.x_max:
        parser.error("require layers >= 1 and finite x-min <= x-max")
    paths = discover(args.root) if args.root.is_dir() else [args.root]
    if not paths:
        parser.error("no source G-code files found")
    results = []
    for path in paths:
        result = check_file(path, args.layers, args.x_min, args.x_max)
        result["motion"] = check_motion_x(path, args.motion_forbidden_x)
        results.append(result)
        bounds = f" X=[{result['x_min']:.3f}, {result['x_max']:.3f}]" if "x_min" in result else ""
        motion = result["motion"]
        print(f"{result['status']:8} motion={motion['status']:5} {path}{bounds}", flush=True)
    counts = dict(Counter(r["status"] for r in results))
    report = {"x_limits_mm": [args.x_min, args.x_max], "layers_including_landing": args.layers,
              "motion_forbidden_x_mm": args.motion_forbidden_x,
              "motion_summary": dict(Counter(r["motion"]["status"] for r in results)),
              "method": "WIDTH/2 capsule connectivity; upper bottom Z touches lower top Z; includes whole touching component",
              "tolerance_mm": TOL, "summary": counts, "files": results}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_markdown(args.report.with_suffix(".md"), report)
    print(f"Summary: {counts}\nMotion summary: {report['motion_summary']}\nReport: {args.report}\nSummary report: {args.report.with_suffix('.md')}")
    return 1 if any(r["status"] in {"FAIL", "UNKNOWN", "ERROR"} or r["motion"]["status"] != "PASS" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())

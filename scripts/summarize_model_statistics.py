#!/usr/bin/env python3
r"""Compare models/<model>/{default,dynapin}/normal/*.gcode.slice_statistics.json.

Run: python3 scripts/summarize_model_statistics.py
Outputs: reports/model_statistics.csv and reports/model_statistics.tex.
The TeX file is a figure* fragment for \input{reports/model_statistics.tex}.
The parent document needs graphicx and caption and figures/fig-slice.png.
The comparison table contains paired values and reduction rates in percent.

Only Python's standard library is required. JSON decimal numbers are converted
to exact fractions: sums, unit conversions and reduction rates incur no binary
floating-point or intermediate rounding. CSV seconds/grams are exact decimal
sums of the stored numbers; CSV percentages use 12 decimal places, and TeX
uses two. Final rounding is half away from zero. This preserves the precision
of the slicer estimates; it does not improve the estimates' physical accuracy.

Support = Support + Support interface + Support transition (absent roles = 0).
Support time is extrusion time for these roles, excluding travel and custom
pin-pull moves. Total time includes preparation and the processor's other
estimated operations. Negative reduction means an increase. A zero baseline
has an undefined percentage (empty CSV cell / -- in TeX).
Missing or multiple sidecars for either condition cause failure, never omission.
Directories whose names contain 'old' (case-insensitive) are excluded before
inspection. Only immediate sidecars in each condition's normal directory are
read; subdirectories (including old archives and experimental variants) are
never traversed. An explicitly supplied models directory named with 'old' is
rejected before it is inspected.
Source paths and SHA-256 hashes in the CSV identify the exact inputs used.
"""

import argparse
import csv
from dataclasses import dataclass
from decimal import Decimal
from fractions import Fraction
import hashlib
import io
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
CONDITIONS = ("default", "dynapin")
SUPPORT_ROLES = {"Support", "Support interface", "Support transition"}
METRICS = ("total_time_s", "support_time_s", "total_filament_g", "support_filament_g")


@dataclass(frozen=True)
class Statistics:
    path: Path
    sha256: str
    values: tuple


@dataclass(frozen=True)
class Comparison:
    model: str
    default: Statistics
    dynapin: Statistics


def fixed(value, places):
    """Round a fraction exactly, including ties and negative values."""
    scale = 10 ** places
    rounded, remainder = divmod(abs(value.numerator) * scale, value.denominator)
    if remainder * 2 >= value.denominator:
        rounded += 1
    sign = "-" if value < 0 and rounded else ""
    if places == 0:
        return f"{sign}{rounded}"
    return f"{sign}{rounded // scale}.{rounded % scale:0{places}d}"


def exact_decimal(value):
    """Write a terminating fraction without discarding any source digits."""
    denominator = value.denominator
    twos = fives = 0
    while denominator % 2 == 0:
        denominator //= 2
        twos += 1
    while denominator % 5 == 0:
        denominator //= 5
        fives += 1
    if denominator != 1:
        raise ValueError("Non-terminating decimal in a raw metric")
    return fixed(value, max(twos, fives))


def reduction(baseline, proposed):
    return (baseline - proposed) / baseline * 100 if baseline else None


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, Decimal)):
        raise ValueError(f"Expected a numeric value, got {value!r}")
    if isinstance(value, Decimal) and not value.is_finite():
        raise ValueError(f"Non-finite value: {value}")
    if value < 0:
        raise ValueError(f"Negative time or mass: {value}")
    return Fraction(value)


def read_statistics(path):
    try:
        raw = path.read_bytes()
        data = json.loads(raw, parse_float=Decimal, parse_constant=Decimal, object_pairs_hook=unique_object)
        total_time = number(data["time"]["normal"]["total_seconds"])
        total_mass = number(data["total_filament"]["weight_g"])
        roles = data["roles"]
        if not isinstance(roles, list) or not roles:
            raise ValueError("Expected a nonempty roles list")
        seen = set()
        support_time = support_mass = Fraction(0)
        for role in roles:
            name = role["role"]
            if not isinstance(name, str) or not name or name in seen:
                raise ValueError(f"Invalid or duplicate role: {name!r}")
            seen.add(name)
            seconds = number(role["time"]["normal"]["seconds"])
            mass = number(role["weight_g"])
            if name.startswith("Support") and name not in SUPPORT_ROLES:
                raise ValueError(f"Unknown support role: {name}")
            if name in SUPPORT_ROLES:
                support_time += seconds
                support_mass += mass
        if support_time > total_time or support_mass > total_mass:
            raise ValueError("Support exceeds the recorded total")
        return Statistics(path, hashlib.sha256(raw).hexdigest(), (total_time, support_time, total_mass, support_mass))
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError(f"{path}: {error}") from error


def collect(models_dir):
    if "old" in models_dir.name.casefold():
        raise ValueError(f"Refusing to inspect an old-data directory: {models_dir}")
    models = sorted(
        path for path in models_dir.iterdir()
        if "old" not in path.name.casefold() and not path.name.startswith(".") and path.is_dir()
    )
    if not models:
        raise ValueError(f"No model directories in {models_dir}")
    comparisons = []
    for model in models:
        statistics = []
        for condition in CONDITIONS:
            directory = model / condition / "normal"
            files = sorted(directory.glob("*.gcode.slice_statistics.json"))
            if len(files) != 1:
                raise ValueError(f"{directory}: expected exactly one statistics JSON, found {len(files)}")
            statistics.append(read_statistics(files[0]))
        comparisons.append(Comparison(model.name, *statistics))
    return comparisons


def render_csv(comparisons, models_dir):
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    header = ["model"]
    for metric in METRICS:
        header.extend([f"default_{metric}", f"dynapin_{metric}", f"{metric}_reduction_pct"])
    for condition in CONDITIONS:
        header.extend([f"{condition}_source_relative_to_models", f"{condition}_source_sha256"])
    writer.writerow(header)
    for pair in comparisons:
        row = [pair.model]
        for baseline, proposed in zip(pair.default.values, pair.dynapin.values):
            percent = reduction(baseline, proposed)
            row.extend([exact_decimal(baseline), exact_decimal(proposed), "" if percent is None else fixed(percent, 12)])
        for statistics in (pair.default, pair.dynapin):
            row.extend([statistics.path.relative_to(models_dir).as_posix(), statistics.sha256])
        writer.writerow(row)
    return output.getvalue()


def tex_escape(text):
    escapes = {
        "\\": r"\textbackslash{}", "&": r"\&", "%": r"\%", "$": r"\$", "#": r"\#",
        "_": r"\_", "{": r"\{", "}": r"\}", "~": r"\textasciitilde{}", "^": r"\textasciicircum{}",
    }
    return "".join(escapes.get(character, character) for character in text)


def render_tex(comparisons):
    """Render a figure/table fragment for inclusion in the paper with input."""
    count = len(comparisons)
    lines = [
        "% Generated by scripts/summarize_model_statistics.py.",
        r"% Requires \usepackage{graphicx} and \usepackage{caption} in the parent document.",
        "% Times: minutes. Filament: grams. Values are slicer estimates, rounded only for display.",
        "% Support = Support + Support interface + Support transition (extrusion only).",
        "% Reduction (%) = (default - dynapin) / default * 100; a zero baseline gives --.",
        "% Exact source values and higher-precision rates are retained in model_statistics.csv.",
        r"\begin{figure*}[p]",
        r"  \centering",
        r"  \includegraphics[width=0.9\textwidth]{figures/fig-slice.png}",
        rf"  \caption{{{count}モデルに対する通常条件とDynaPin条件のスライス結果．}}",
        r"  \label{fig:fig-slice}",
        r"  \vspace{4mm}",
        r"  \scriptsize",
        r"  \setlength{\tabcolsep}{3pt}",
        r"  \renewcommand{\arraystretch}{1.15}",
        r"  \captionof{table}{時間は分，材料使用量はgで示す．削減率は通常条件を基準とし，負の値は増加を表す．通常条件が0の場合の削減率は--で示す．}",
        r"  \label{tab:tab-slice}",
        r"  \begin{tabular}{l|rrr|rrr|rrr|rrr}",
        r"    \hline",
        r"    & \multicolumn{3}{c|}{総造形時間} & \multicolumn{3}{c|}{サポート相当造形時間}"
        r" & \multicolumn{3}{c|}{総材料使用量} & \multicolumn{3}{c}{サポート相当材料使用量} \\",
        r"    モデル & 通常 & DynaPin & 削減率 [\%] & 通常 & DynaPin & 削減率 [\%]"
        r" & 通常 & DynaPin & 削減率 [\%] & 通常 & DynaPin & 削減率 [\%] \\",
        r"    \hline",
    ]
    for pair in comparisons:
        cells = [tex_escape(pair.model)]
        for index in range(len(METRICS)):
            divisor = 60 if index < 2 else 1
            cells.extend(fixed(statistics.values[index] / divisor, 2) for statistics in (pair.default, pair.dynapin))
            percent = reduction(pair.default.values[index], pair.dynapin.values[index])
            cells.append("--" if percent is None else fixed(percent, 2))
        lines.append("    " + " & ".join(cells) + r" \\")
    lines.extend([r"    \hline", r"  \end{tabular}", r"\end{figure*}"])
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--models-dir", type=Path, default=ROOT / "models")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "reports")
    args = parser.parse_args(argv)
    try:
        comparisons = collect(args.models_dir)
        # Validate and render both artifacts before replacing any existing outputs.
        csv_text = render_csv(comparisons, args.models_dir)
        tex_text = render_tex(comparisons)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for filename, content in (("model_statistics.csv", csv_text), ("model_statistics.tex", tex_text)):
            path = args.output_dir / filename
            path.write_text(content, encoding="utf-8")
            print(path)
        print(f"Compared {len(comparisons)} models ({2 * len(comparisons)} statistics files).")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

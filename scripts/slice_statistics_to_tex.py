#!/usr/bin/env python3
"""Export models/<model>/{normal,dynapin}/<model>.gcode.slice_statistics.json.

Usage: python3 scripts/slice_statistics_to_tex.py -o slicing-results.tex
The generated fragment requires graphicx and caption in the parent document.
Times are minutes; filament amounts are grams. Missing files produce -- cells.
"""

import argparse
import json
from pathlib import Path
import sys


MODELS = (
    ("Overhang_Angle_Bracket", "Angle Bracket"),
    ("RemoveGymnastBase", "Gymnast"),
    ("Cheburashka", "Cheburashka"),
    ("3D_Fiducial_Marker", "Fiducial Marker"),
    ("Stanford_Bunny", "Stanford Bunny"),
    ("Armadillo", "Armadillo"),
    ("Simple_Bridge", "Simple Bridge"),
    ("Enterprise", "Enterprise"),
    ("Nefertiti", "Nefertiti"),
    ("Beast", "Beast"),
    ("Cow", "Cow"),
    ("Angled_Cylinder", "Angled Cylinder"),
    ("Arch_Bridge", "Arch Bridge"),
    ("Rocker_Arm", "Rocker Arm"),
    ("bike-helmet", "Bike Helmet"),
)
SUPPORT_ROLES = {"Support", "Support interface", "Support transition"}


def read_metrics(path):
    """Return total/support time (min), then total/support filament (g)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    support = [role for role in data["roles"] if role["role"] in SUPPORT_ROLES]
    return (
        data["time"]["normal"]["total_seconds"] / 60,
        sum(role["time"]["normal"]["seconds"] for role in support) / 60,
        data["total_filament"]["weight_g"],
        sum(role["weight_g"] for role in support),
    )


def render_tex(models_dir):
    lines = [
        r"% Requires \usepackage{graphicx} and \usepackage{caption}.",
        "% Times: minutes. Filament: grams. Support includes Support, Support interface, Support transition.",
        r"\begin{figure*}[p]",
        r"  \centering",
        r"  \includegraphics[width=0.9\textwidth]{figures/fig-slice.png}",
        r"  \caption{15モデルに対するOrcaSlicerのスライス結果．}",
        r"  \label{fig:fig-slice}",
        r"  \vspace{4mm}",
        r"  \scriptsize",
        r"  \setlength{\tabcolsep}{3pt}",
        r"  \renewcommand{\arraystretch}{1.15}",
        r"  \captionof{table}{15モデルに対する通常サポート条件とDynaPin条件のスライス結果．}",
        r"  \label{tab:slicing-results}",
        r"  \begin{tabular}{l|cc|cc|cc|cc}",
        r"    \hline",
        r"    & \multicolumn{2}{c|}{総造形時間 [min]} & \multicolumn{2}{c|}{サポート相当造形時間 [min]} & \multicolumn{2}{c|}{総フィラメント使用量 [g]} & \multicolumn{2}{c}{サポート相当フィラメント使用量 [g]} \\",
        r"    モデル & 通常 & DynaPin & 通常 & DynaPin & 通常 & DynaPin & 通常 & DynaPin \\",
        r"    \hline",
    ]
    for model, label in MODELS:
        conditions = []
        for condition in ("normal", "dynapin"):
            path = models_dir / model / condition / f"{model}.gcode.slice_statistics.json"
            if not path.is_file():
                print(f"Missing statistics: {path}", file=sys.stderr)
                conditions.append(["--"] * 4)
                continue
            try:
                conditions.append([f"{value:.2f}" for value in read_metrics(path)])
            except (OSError, ValueError, KeyError, TypeError) as error:
                raise ValueError(f"{path}: {error}") from error
        cells = [conditions[c][metric] for metric in range(4) for c in range(2)]
        lines.append("    " + " & ".join([label] + cells) + r" \\")
    lines.extend([r"    \hline", r"  \end{tabular}", r"\end{figure*}"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-dir", type=Path, default=Path(__file__).resolve().parents[1] / "models")
    parser.add_argument("-o", "--output", type=Path, help="Output TeX path (default: stdout)")
    args = parser.parse_args()
    if not args.models_dir.is_dir():
        parser.error(f"Models directory does not exist: {args.models_dir}")
    try:
        tex = render_tex(args.models_dir)
        if args.output:
            args.output.write_text(tex, encoding="utf-8")
        else:
            sys.stdout.write(tex)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

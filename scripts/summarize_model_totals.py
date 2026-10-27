#!/usr/bin/env python3
"""Sum the unrounded values in model_statistics.csv and report aggregate savings.

Run: python3 scripts/summarize_model_totals.py
Input: reports/model_statistics.csv (regenerate it after replacing model data).
Output: reports/model_statistics_totals.csv, plus a readable terminal table.

Times are summed in seconds before conversion to minutes; masses are in grams.
Reduction is (sum(default) - sum(dynapin)) / sum(default) * 100, not the mean
of per-model percentages. All calculations use exact fractions. CSV minutes
and percentages are rounded to 12 decimal places; gram values retain their
exact decimal sums. Terminal values are rounded to two decimal places.
A zero baseline has an undefined reduction (empty CSV cell / -- in terminal).
Rows whose model or source directory names contain 'old' are excluded.
The script only reads the input CSV and does not traverse model directories.
"""

import argparse
import csv
from decimal import Decimal
from fractions import Fraction
import io
from pathlib import Path
import sys

from summarize_model_statistics import CONDITIONS, METRICS, ROOT, exact_decimal, fixed, number, reduction


LABELS = ("総造形時間 [min]", "サポート相当造形時間 [min]", "総材料使用量 [g]", "サポート相当材料使用量 [g]")


def read_totals(path):
    totals = {condition: [Fraction(0) for _ in METRICS] for condition in CONDITIONS}
    models = set()
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            model = row["model"]
            if "old" in model.casefold() or any(
                "old" in part.casefold()
                for condition in CONDITIONS
                for part in Path(row[f"{condition}_source_relative_to_models"]).parts[:-1]
            ):
                print(f"Excluded old-data row: {model}", file=sys.stderr)
                continue
            if not model or model in models:
                raise ValueError(f"Empty or duplicate model name: {model!r}")
            models.add(model)
            for condition in CONDITIONS:
                for index, metric in enumerate(METRICS):
                    totals[condition][index] += number(Decimal(row[f"{condition}_{metric}"]))
    if not models:
        raise ValueError(f"No current model rows in {path}")
    return len(models), totals


def render_totals(model_count, totals):
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(["metric", "unit", "model_count", "default", "dynapin", "saved", "reduction_pct"])
    lines = [
        f"対象モデル数: {model_count}",
        "| 指標 | 通常 | DynaPin | 削減量 | 削減率 [%] |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for index, metric in enumerate(METRICS):
        baseline, proposed = (totals[condition][index] for condition in CONDITIONS)
        percent = reduction(baseline, proposed)
        values = (baseline, proposed, baseline - proposed)
        is_time = index < 2
        displayed = tuple(value / 60 if is_time else value for value in values)
        writer.writerow([
            metric.removesuffix("_s" if is_time else "_g"),
            "min" if is_time else "g", model_count,
            *(fixed(value, 12) if is_time else exact_decimal(value) for value in displayed),
            "" if percent is None else fixed(percent, 12),
        ])
        cells = [LABELS[index], *(fixed(value, 2) for value in displayed), "--" if percent is None else fixed(percent, 2)]
        lines.append("| " + " | ".join(cells) + " |")
    return output.getvalue(), "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, default=ROOT / "reports" / "model_statistics.csv")
    parser.add_argument("--output", type=Path, default=ROOT / "reports" / "model_statistics_totals.csv")
    args = parser.parse_args(argv)
    try:
        if args.input.resolve() == args.output.resolve():
            raise ValueError("Input and output must be different files")
        count, totals = read_totals(args.input)
        csv_text, table = render_totals(count, totals)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(csv_text, encoding="utf-8")
        print(table)
        print(f"\n出力: {args.output}")
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

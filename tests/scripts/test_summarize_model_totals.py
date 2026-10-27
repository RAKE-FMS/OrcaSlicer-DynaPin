import csv
from fractions import Fraction
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import summarize_model_totals as totals


class ModelTotalsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "models.csv"

    def write_rows(self, observations):
        fields = ["model", *(f"{condition}_{metric}" for condition in totals.CONDITIONS for metric in totals.METRICS),
                  *(f"{condition}_source_relative_to_models" for condition in totals.CONDITIONS)]
        with self.path.open("w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            for model, baseline, proposed in observations:
                row = {"model": model}
                for condition, values in zip(totals.CONDITIONS, (baseline, proposed)):
                    row[f"{condition}_source_relative_to_models"] = f"{model}/{condition}/normal/result.gcode.slice_statistics.json"
                    row.update((f"{condition}_{metric}", value) for metric, value in zip(totals.METRICS, values))
                writer.writerow(row)

    def test_rates_use_summed_values_and_unit_conversion_follows_summation(self):
        self.write_rows([
            ("small", ("60.3", "10", "100", "0.004"), ("30", "5", "50", "0")),
            ("large", ("59.7", "20", "900", "0.004"), ("30", "10", "810", "0")),
        ])
        count, values = totals.read_totals(self.path)
        self.assertEqual(count, 2)
        self.assertEqual(values["default"][0], 120)
        self.assertEqual(values["default"][3], Fraction("0.008"))
        content, table = totals.render_totals(count, values)
        rows = {row["metric"]: row for row in csv.DictReader(io.StringIO(content))}
        self.assertEqual(rows["total_time"]["default"], "2.000000000000")
        self.assertEqual(rows["total_filament"]["reduction_pct"], "14.000000000000")
        self.assertEqual(rows["support_filament"]["default"], "0.008")
        self.assertIn("| サポート相当材料使用量 [g] | 0.01 |", table)

    def test_zero_baselines_and_increases(self):
        self.write_rows([("part", ("60", "0", "10", "0"), ("90", "1", "12", "1"))])
        content, table = totals.render_totals(*totals.read_totals(self.path))
        rows = list(csv.DictReader(io.StringIO(content)))
        self.assertEqual(rows[0]["saved"], "-0.500000000000")
        self.assertEqual(rows[0]["reduction_pct"], "-50.000000000000")
        self.assertEqual(rows[1]["reduction_pct"], "")
        self.assertIn("--", table)

    def test_old_rows_are_excluded_and_duplicate_or_invalid_rows_fail(self):
        self.write_rows([("part", (1, 0, 1, 0), (1, 0, 1, 0)),
                         ("archive_OLD", ("invalid",) * 4, ("invalid",) * 4)])
        self.assertEqual(totals.read_totals(self.path)[0], 1)
        self.write_rows([("part", (1, 0, 1, 0), (1, 0, 1, 0))] * 2)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            totals.read_totals(self.path)
        for invalid in ("NaN", "-1", "Infinity"):
            self.write_rows([("part", (invalid, 0, 1, 0), (1, 0, 1, 0))])
            with self.assertRaises(ValueError):
                totals.read_totals(self.path)

    def test_cli_exports_totals_and_rejects_overwriting_input(self):
        self.write_rows([("part", (60, 30, 10, 5), (30, 15, 5, 2))])
        script = Path(totals.__file__)
        output = self.path.with_name("totals.csv")
        command = [sys.executable, str(script), "--input", str(self.path), "--output"]
        result = subprocess.run([*command, str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open() as source:
            self.assertEqual(len(list(csv.DictReader(source))), 4)
        original = self.path.read_bytes()
        result = subprocess.run([*command, str(self.path)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()

import csv
from decimal import Decimal
from fractions import Fraction
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "summarize_model_statistics.py"
spec = importlib.util.spec_from_file_location("summarize_model_statistics", SCRIPT)
summary = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = summary
spec.loader.exec_module(summary)


def fixture():
    return {
        "time": {"normal": {"total_seconds": 120.1, "total_display": "WRONG"}},
        "total_filament": {"weight_g": 10.1, "weight_display": "WRONG"},
        "roles": [
            {"role": name, "time": {"normal": {"seconds": seconds}}, "weight_g": weight}
            for name, seconds, weight in (
                ("Support", 0.1, 0.1), ("Support interface", 0.2, 0.2),
                ("Support transition", 0.3, 0.3), ("Custom", 20, 3), ("Inner wall", 40, 6),
            )
        ],
    }


class ModelStatisticsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.paths = []
        for condition in ("default", "dynapin"):
            path = self.root / "models" / "part_1" / condition / "normal" / "result.gcode.slice_statistics.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(fixture()), encoding="utf-8")
            self.paths.append(path)

    def write(self, data):
        self.paths[0].write_text(json.dumps(data), encoding="utf-8")

    def test_exact_raw_metrics_and_support_definition(self):
        result = summary.read_statistics(self.paths[0])
        self.assertEqual(result.values, (Fraction("120.1"), Fraction("0.6"), Fraction("10.1"), Fraction("0.6")))
        self.assertEqual(summary.exact_decimal(result.values[1]), "0.6")
        self.assertEqual(result.sha256, hashlib.sha256(self.paths[0].read_bytes()).hexdigest())

    def test_negative_reduction_zero_baseline_and_exact_rounding(self):
        self.assertEqual(summary.reduction(Fraction(100), Fraction(125)), -25)
        self.assertIsNone(summary.reduction(Fraction(0), Fraction(0)))
        self.assertIsNone(summary.reduction(Fraction(0), Fraction(1)))
        for value, expected in (("1.005", "1.01"), ("-1.005", "-1.01"), ("-0.001", "0.00")):
            self.assertEqual(summary.fixed(Fraction(value), 2), expected)
        self.assertEqual(summary.fixed(Fraction(1, 3), 12), "0.333333333333")

    def test_non_support_custom_time_is_excluded(self):
        data = fixture()
        data["roles"] = data["roles"][3:]
        self.write(data)
        self.assertEqual(summary.read_statistics(self.paths[0]).values[1::2], (0, 0))

    def test_rejects_invalid_numeric_values(self):
        for value in (-1, True, "12", None, float("nan"), float("inf")):
            with self.subTest(value=value):
                data = fixture()
                data["roles"][0]["weight_g"] = value
                self.write(data)
                with self.assertRaisesRegex(ValueError, str(self.paths[0])):
                    summary.read_statistics(self.paths[0])

    def test_rejects_missing_fields_empty_roles_duplicates_and_unknown_support(self):
        invalid = []
        data = fixture()
        del data["roles"][0]["time"]
        invalid.append(data)
        data = fixture()
        data["roles"] = []
        invalid.append(data)
        data = fixture()
        data["roles"].append(data["roles"][0])
        invalid.append(data)
        data = fixture()
        data["roles"][0]["role"] = "Support new kind"
        invalid.append(data)
        data = fixture()
        data["total_filament"]["weight_g"] = 0.01
        invalid.append(data)
        for data in invalid:
            self.write(data)
            with self.assertRaises(ValueError):
                summary.read_statistics(self.paths[0])
        self.paths[0].write_text('{"roles": [], "roles": []}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate JSON key"):
            summary.read_statistics(self.paths[0])

    def test_missing_and_ambiguous_statistics_fail(self):
        duplicate = self.paths[0].with_name("other.gcode.slice_statistics.json")
        duplicate.write_bytes(self.paths[0].read_bytes())
        with self.assertRaisesRegex(ValueError, "found 2"):
            summary.collect(self.root / "models")
        duplicate.unlink()
        self.paths[0].unlink()
        with self.assertRaisesRegex(ValueError, "found 0"):
            summary.collect(self.root / "models")

    def test_old_directories_are_never_inspected_or_read(self):
        models = self.root / "models"
        for name in ("old", "archive_OLD", "part_old", "old_backup"):
            (models / name).mkdir()
            for path in self.paths:
                archive = path.parent / name
                archive.mkdir()
                (archive / "old.gcode.slice_statistics.json").write_text("invalid JSON", encoding="utf-8")

        original_iterdir = Path.iterdir
        original_read_bytes = Path.read_bytes
        original_is_dir = Path.is_dir

        def guard(path):
            self.assertFalse(any("old" in part.casefold() for part in path.relative_to(models).parts))

        def checked_iterdir(path):
            guard(path)
            return original_iterdir(path)

        def checked_read_bytes(path):
            guard(path)
            return original_read_bytes(path)

        def checked_is_dir(path):
            guard(path)
            return original_is_dir(path)

        with patch.object(Path, "iterdir", checked_iterdir), patch.object(Path, "read_bytes", checked_read_bytes), \
                patch.object(Path, "is_dir", checked_is_dir):
            pairs = summary.collect(models)
        self.assertEqual([pair.model for pair in pairs], ["part_1"])

    def test_old_input_root_is_rejected_before_inspection(self):
        with patch.object(Path, "iterdir") as inspect:
            with self.assertRaisesRegex(ValueError, "Refusing to inspect"):
                summary.collect(self.root / "models_OLD")
        inspect.assert_not_called()

    def test_csv_and_tex_fragment_use_same_unrounded_inputs(self):
        data = fixture()
        data["time"]["normal"]["total_seconds"] = 150.125
        self.paths[1].write_text(json.dumps(data), encoding="utf-8")
        pairs = summary.collect(self.root / "models")
        row = next(csv.DictReader(io.StringIO(summary.render_csv(pairs, self.root / "models"))))
        self.assertEqual(row["default_total_time_s"], "120.1")
        self.assertEqual(row["dynapin_total_time_s"], "150.125")
        self.assertEqual(row["total_time_s_reduction_pct"], "-25.000000000000")
        self.assertEqual(Decimal(row["default_support_filament_g"]), Decimal("0.6"))
        tex = summary.render_tex(pairs)
        self.assertIn(r"\begin{figure*}[p]", tex)
        self.assertIn(r"\end{figure*}", tex)
        self.assertNotIn(r"\documentclass", tex)
        self.assertNotIn(r"\begin{document}", tex)
        self.assertIn(r"\includegraphics[width=0.9\textwidth]{figures/fig-slice.png}", tex)
        self.assertIn(r"\captionof{table}", tex)
        self.assertIn(r"\begin{tabular}{l|rrr|rrr|rrr|rrr}", tex)
        self.assertEqual(tex.count(r"削減率 [\%]"), 4)
        self.assertIn(r"part\_1 & 2.00 & 2.50 & -25.00 & 0.01 & 0.01 & 0.00 & 10.10 & 10.10 & 0.00 & 0.60 & 0.60 & 0.00", tex)
        self.assertNotIn("\t", tex)

    def test_zero_baseline_has_blank_csv_rate_and_dash_tex_rate(self):
        data = fixture()
        data["roles"] = data["roles"][3:]
        self.write(data)
        pairs = summary.collect(self.root / "models")
        row = next(csv.DictReader(io.StringIO(summary.render_csv(pairs, self.root / "models"))))
        self.assertEqual(row["support_time_s_reduction_pct"], "")
        self.assertIn("2.00 & 2.00 & 0.00 & 0.00 & 0.01 & -- & 10.10 & 10.10 & 0.00 & 0.00 & 0.60 & --", summary.render_tex(pairs))

    def test_cli_writes_both_outputs_and_preserves_them_on_invalid_input(self):
        output = self.root / "output"
        command = [sys.executable, str(SCRIPT), "--models-dir", str(self.root / "models"), "--output-dir", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Compared 1 models", result.stdout)
        before = {path.name: path.read_bytes() for path in output.iterdir()}
        self.paths[0].unlink()
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, {path.name: path.read_bytes() for path in output.iterdir()})


if __name__ == "__main__":
    unittest.main()

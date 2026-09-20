import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "extract_dynapin_min_gcode.py"
SPEC = importlib.util.spec_from_file_location("extract_dynapin_min_gcode", SCRIPT_PATH)
module = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)


def make_source(pull_layers, layer_count, endings=None):
    lines = []
    for layer_index in range(layer_count):
        lines.extend(
            [
                b";LAYER_CHANGE",
                f";Z:{layer_index}".encode(),
                f"LAYER_{layer_index}".encode(),
            ]
        )
        for label in pull_layers.get(layer_index, []):
            lines.extend(
                [
                    f"; BEGIN_DYNAPIN_PULL {label}".encode(),
                    f"PULL_{label}".encode(),
                    b"; END_DYNAPIN_PULL",
                ]
            )

    if endings is None:
        endings = [b"\n"] * len(lines)
    return b"".join(line + endings[index % len(endings)] for index, line in enumerate(lines))


class ExtractMinGcodeTests(unittest.TestCase):
    def test_multiple_pull_blocks_in_one_layer_are_kept_in_order(self):
        source = make_source({1: ["A", "B"]}, layer_count=6)

        result = module.extract_min_payload(b"BASE\n", source)

        self.assertEqual(
            result,
            b"BASE\n"
            b"; BEGIN_DYNAPIN_PULL A\nPULL_A\n; END_DYNAPIN_PULL\n"
            b"; BEGIN_DYNAPIN_PULL B\nPULL_B\n; END_DYNAPIN_PULL\n"
            b";LAYER_CHANGE\n;Z:2\nLAYER_2\n"
            b";LAYER_CHANGE\n;Z:3\nLAYER_3\n"
            b";LAYER_CHANGE\n;Z:4\nLAYER_4\n"
            b";LAYER_CHANGE\n;Z:5\nLAYER_5\n",
        )

    def test_pull_layers_are_emitted_in_source_order(self):
        source = make_source({1: ["A"], 6: ["B"]}, layer_count=11)

        result = module.extract_min_payload(b"BASE\n", source)

        self.assertLess(result.index(b"PULL_A"), result.index(b"LAYER_2"))
        self.assertLess(result.index(b"LAYER_5"), result.index(b"PULL_B"))
        self.assertEqual(result.count(b";LAYER_CHANGE\n"), 8)
        self.assertNotIn(b"LAYER_1\n", result)
        self.assertNotIn(b"LAYER_6\n", result)

    def test_overlapping_following_layer_windows_are_not_duplicated(self):
        source = make_source({1: ["A"], 3: ["B"]}, layer_count=8)

        result = module.extract_min_payload(b"BASE\n", source)

        self.assertIn(b"LAYER_3\n; BEGIN_DYNAPIN_PULL B", result)
        for layer_index in range(2, 8):
            self.assertEqual(result.count(f"LAYER_{layer_index}\n".encode()), 1)

    def test_source_line_endings_are_preserved(self):
        endings = [b"\r\n", b"\n", b"\r\n", b"\n"]
        source = make_source({1: ["A"]}, layer_count=6, endings=endings)

        result = module.extract_min_payload(b"BASE\r\n", source)

        expected_source = b"".join(
            line
            for line in source.splitlines(keepends=True)
            if b"PULL_A" in line or b"BEGIN_DYNAPIN_PULL A" in line or b"END_DYNAPIN_PULL" in line
        )
        self.assertTrue(result.startswith(b"BASE\r\n"))
        self.assertIn(b"; BEGIN_DYNAPIN_PULL A\r\n", result)
        self.assertIn(b";LAYER_CHANGE\n", result)
        self.assertTrue(result.endswith(source.splitlines(keepends=True)[-1]))
        self.assertEqual(result.count(b"PULL_A"), expected_source.count(b"PULL_A"))

    def test_base_without_line_ending_gets_one_separator(self):
        source = make_source({1: ["A"]}, layer_count=6)

        result = module.extract_min_payload(b"BASE", source)

        self.assertTrue(result.startswith(b"BASE\n; BEGIN_DYNAPIN_PULL A\n"))

    def test_no_markers_returns_none(self):
        source = make_source({}, layer_count=6)

        self.assertIsNone(module.extract_min_payload(b"BASE\n", source))

    def test_unbalanced_markers_raise_extraction_error(self):
        source = b";LAYER_CHANGE\n;Z:0\n; BEGIN_DYNAPIN_PULL A\nPULL_A\n"

        with self.assertRaises(module.ExtractionError):
            module.extract_min_payload(b"BASE\n", source)

    def test_insufficient_following_layers_raise_extraction_error(self):
        source = make_source({1: ["A"]}, layer_count=5)

        with self.assertRaises(module.ExtractionError):
            module.extract_min_payload(b"BASE\n", source)

    def test_discover_gcodes_is_recursive_and_skips_generated_files(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            outputs_dir = Path(temp_dir) / "outputs"
            (outputs_dir / "nested").mkdir(parents=True)
            (outputs_dir / "a.gcode").write_bytes(b"")
            (outputs_dir / "nested" / "b.GCODE").write_bytes(b"")
            (outputs_dir / "min_a.gcode").write_bytes(b"")
            (outputs_dir / "nested" / "min_b.GCODE").write_bytes(b"")
            (outputs_dir / "readme.txt").write_bytes(b"")

            discovered = module.discover_gcodes(outputs_dir)

            self.assertEqual(discovered, [outputs_dir / "a.gcode", outputs_dir / "nested" / "b.GCODE"])

    def test_cli_writes_sibling_output_and_does_not_reprocess_it(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            models_dir = Path(temp_dir) / "models"
            outputs_dir = models_dir.parent / "outputs" / "nested"
            outputs_dir.mkdir(parents=True)
            (models_dir / "min").mkdir(parents=True)
            (models_dir / "min" / "min.gcode").write_bytes(b"BASE\n")
            source_path = outputs_dir / "part.gcode"
            source_path.write_bytes(make_source({1: ["A"]}, layer_count=6))

            self.assertEqual(module.main(["--models-dir", str(models_dir)]), 0)
            output_path = outputs_dir / "min_part.gcode"
            self.assertTrue(output_path.exists())
            self.assertTrue(output_path.read_bytes().startswith(b"BASE\n; BEGIN_DYNAPIN_PULL A\n"))

            self.assertEqual(module.main(["--models-dir", str(models_dir)]), 0)
            self.assertFalse((outputs_dir / "min_min_part.gcode").exists())


if __name__ == "__main__":
    unittest.main()

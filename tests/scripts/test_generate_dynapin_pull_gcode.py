import copy
import importlib.util
import io
import json
import re
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("generate_dynapin_pull_gcode", ROOT / "scripts/generate_dynapin_pull_gcode.py")
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


def moves(gcode):
    return [line for line in gcode.splitlines() if line.startswith("G1 ")]


class GeneratePullGcodeTests(unittest.TestCase):
    def setUp(self):
        self.config = module.load_config(module.DEFAULT_CONFIG)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.base_file = Path(directory.name) / "min.gcode"
        self.base_file.write_bytes(b"G28\r\n; base G-code\r\n")

    def test_current_matches_existing_cpp_pin_block(self):
        # Independent reference captured from DynaPin.cpp with the original KP3S profile.
        self.assertEqual(module.pull_gcode_for_pin(self.config, (0, 0), "current"),
                         "; BEGIN_DYNAPIN_PULL ROW=0 COL=0\n"
                         "G1 Y6.5000 F5000.0000\n"
                         "G1 X169.0000 Z6.0000 F5000.0000\n"
                         "G1 X174.0000 F1500.0000\n"
                         "G1 Y10.5000 F1500.0000\n"
                         "; DYNAPIN_PULL_MOVE\n"
                         "G1 X20.0000 F3000.0000\n"
                         "G1 X21.5000 Y6.5000 F1500.0000\n"
                         "G1 Z10.0000 F1500.0000\n"
                         "; END_DYNAPIN_PULL\n")

    def test_alternating_even_row_uses_reverted_z_motion_without_retreat(self):
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (0, 0), "alternating")), [
            "G1 Y1.9000 F5000.0000",
            "G1 X169.0000 Z11.0000 F5000.0000",
            "G1 X174.0000 F1500.0000",
            "G1 Z6.0000 F1500.0000",
            "G1 X20.0000 F3000.0000",
            "G1 X21.5000 Z11.0000 F1500.0000",
        ])

    def test_alternating_normal_row_uses_configured_y_offsets_and_row_col_pitch(self):
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (1, 2), "alternating")), [
            "G1 Y35.5000 F5000.0000",
            "G1 X169.0000 Z18.4000 F5000.0000",
            "G1 X174.0000 F1500.0000",
            "G1 Z13.4000 F1500.0000",
            "G1 X20.0000 F3000.0000",
            "G1 X21.5000 Z18.4000 F1500.0000",
        ])

    def test_parity_one_swaps_row_assignments(self):
        self.config["pull_gcode"]["alternating"]["row_parity"] = 1
        even = moves(module.pull_gcode_for_pin(self.config, (0, 0), "alternating"))
        odd = moves(module.pull_gcode_for_pin(self.config, (1, 0), "alternating"))
        self.assertEqual(even[0], "G1 Y10.7000 F5000.0000")
        self.assertEqual(odd[0], "G1 Y1.9000 F5000.0000")

    def test_reverted_swaps_approach_and_engagement_without_mirroring(self):
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (0, 0), "reverted")), [
            "G1 Y10.5000 F5000.0000",
            "G1 X169.0000 Z6.0000 F5000.0000",
            "G1 X174.0000 F1500.0000",
            "G1 Y6.5000 F1500.0000",
            "G1 X20.0000 F3000.0000",
            "G1 X21.5000 Y10.5000 F1500.0000",
            "G1 Z10.0000 F1500.0000",
        ])

    def test_normal_uses_normal_offset_for_every_row(self):
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (0, 0), "normal"))[0],
                         "G1 Y13.5000 F5000.0000")
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (1, 0), "normal"))[0],
                         "G1 Y13.5000 F5000.0000")

    def test_standalone_mode_profiles_do_not_change_alternating_profiles(self):
        alternating_before = moves(module.pull_gcode_for_pin(self.config, (1, 0), "alternating"))
        self.config["pull_gcode"]["normal"].update(y_origin_offset=20., x_hook=140.)
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (1, 0), "alternating")), alternating_before)
        normal_moves = moves(module.pull_gcode_for_pin(self.config, (1, 0), "normal"))
        self.assertEqual(normal_moves[0], "G1 Y26.5000 F5000.0000")
        self.assertEqual(normal_moves[1], "G1 X140.0000 Z13.4000 F5000.0000")

    def test_mode_and_alternating_orientation_parameters_are_independent(self):
        alternating = self.config["pull_gcode"]["alternating"]
        normal = alternating["normal"]
        reverted = alternating["reverted"]
        normal.update(alternating_y_offset=-2., approach_z_offset=6.)
        reverted.update(alternating_y_offset=-3., approach_z_offset=8.)
        normal.update(x_hook=150., fast_feed_rate=4100., feed_rate=1100.)
        reverted.update(x_hook=150., fast_feed_rate=4100., feed_rate=1100.)
        normal_moves = moves(module.pull_gcode_for_pin(self.config, (1, 0), "alternating"))
        reverted_moves = moves(module.pull_gcode_for_pin(self.config, (0, 0), "alternating"))
        self.assertEqual(normal_moves[0], "G1 Y9.7000 F4100.0000")
        self.assertEqual(normal_moves[1], "G1 X150.0000 Z19.4000 F4100.0000")
        self.assertEqual(reverted_moves[0], "G1 Y-0.1000 F4100.0000")
        self.assertEqual(reverted_moves[1], "G1 X150.0000 Z14.0000 F4100.0000")
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (0, 0), "normal"))[0],
                         "G1 Y13.5000 F5000.0000")
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (0, 0), "reverted"))[0],
                         "G1 Y10.5000 F5000.0000")
        for last_pin in ((1, 0), (2, 0)):
            result = module.generate_gcode(self.config, [(0, 0), last_pin], return_position=(100., 120., 30.))
            self.assertEqual(moves(result)[-2:], [
                "G1 X100.0000 Y120.0000 F4100.0000", "G1 Z30.0000 F1100.0000",
            ])

    def test_configured_coordinates_and_feedrates_control_motion(self):
        grid = self.config["grid"]
        grid["pull_origin"] = {"y": 40., "z": 20.}
        grid["pitch"] = {"col_y": 10., "row_z": 8.}
        alternating_normal = self.config["pull_gcode"]["alternating"]["normal"]
        alternating_normal["y_origin_offset"] = 9.
        alternating_normal.update({
            "x_hook": 160., "x_latch": 166., "x_front": 30.,
            "fast_feed_rate": 4000., "feed_rate": 1000., "pull_feedrate_fast": 2000.,
        })
        alternating_normal.update({
            "y_offset": -2., "approach_y_offset": -3., "approach_z_offset": 6.,
            "engage_z_drop": 4., "disengage_z_lift": 3., "disengage_x_offset": 2.,
            "retreat_z_lift": 80.,
        })
        self.assertEqual(moves(module.pull_gcode_for_pin(self.config, (1, 2), "alternating")), [
            "G1 Y63.0000 F4000.0000",
            "G1 X160.0000 Z34.0000 F4000.0000",
            "G1 X166.0000 F1000.0000",
            "G1 Z30.0000 F1000.0000",
            "G1 X30.0000 F2000.0000",
            "G1 X32.0000 Z33.0000 F1000.0000",
        ])

    def test_selected_pins_are_sorted_and_deduplicated_with_one_return(self):
        gcode = module.generate_gcode(self.config, [(2, 1), (0, 2), (0, 0), (2, 1)],
                                     return_position=(100., 120., 30.))
        self.assertEqual(re.findall(r"BEGIN_DYNAPIN_PULL ROW=(\d+) COL=(\d+)", gcode),
                         [("0", "0"), ("0", "2"), ("2", "1")])
        self.assertTrue(gcode.startswith("; DynaPin Python pull verification TYPE=alternating\nG21\nG90\n"))
        self.assertEqual(gcode.count("BEGIN_DYNAPIN_RETURN"), 1)
        self.assertTrue(gcode.endswith("; BEGIN_DYNAPIN_RETURN\n"
                                       "G1 X100.0000 Y120.0000 F5000.0000\n"
                                       "G1 Z30.0000 F1500.0000\n"
                                       "; END_DYNAPIN_RETURN\n"))

    def test_cpp_reference_fixture_matches_python(self):
        fixtures = json.loads((ROOT / "tests/data/dynapin/pull_gcode_reference.json").read_text())
        for fixture in fixtures:
            config = fixture["config"]
            for case in fixture["cases"]:
                config["pull_gcode"]["alternating"]["row_parity"] = case["row_parity"]
                with self.subTest(mode=case["type"], parity=case["row_parity"], pin=case["pin"]):
                    self.assertEqual(module.pull_gcode_for_pin(config, tuple(case["pin"]), case["type"]), case["gcode"])

    def test_invalid_pins_are_rejected(self):
        for pin in [(-1, 0), (0, -1), (10, 0), (0, 14)]:
            with self.subTest(pin=pin), self.assertRaises(ValueError):
                module.generate_gcode(self.config, [pin])
        with self.assertRaises(ValueError):
            module.parse_pins("0,0;wrong")
        with self.assertRaises(ValueError):
            module.generate_gcode(self.config, [])

    def test_cli_uses_supplied_json_and_type_without_default_path_dependency(self):
        self.config["pull_gcode"]["type"] = "reverted"
        self.config["pull_gcode"]["reverted"]["x_hook"] = 150.
        with tempfile.TemporaryDirectory() as temp:
            config_path = Path(temp) / "config.json"
            output_path = Path(temp) / "pull.gcode"
            config_path.write_text(json.dumps(self.config), encoding="utf-8")
            self.assertEqual(module.main(["--config", str(config_path), "--pins", "1,2;0,0",
                                          "--base-file", str(self.base_file),
                                          "--output", str(output_path)]), 0)
            gcode = output_path.read_text()
            self.assertIn("TYPE=reverted", gcode)
            self.assertIn("G1 X150.0000 Z6.0000", gcode)
            self.assertEqual(moves(gcode)[0], "G1 Y10.5000 F5000.0000")
            buffer = io.BytesIO()
            stdout = io.TextIOWrapper(buffer, encoding="utf-8")
            with redirect_stdout(stdout):
                self.assertEqual(module.main(["--config", str(config_path), "--base-file", str(self.base_file),
                                              "--pins", "0,0", "--current"]), 0)
            self.assertEqual(moves(buffer.getvalue().decode("utf-8"))[0], "G1 Y6.5000 F5000.0000")

    def test_base_bytes_are_copied_before_motion_for_file_and_stdout(self):
        for base in (b"\xef\xbb\xbf;\xe5\x88\x9d\xe6\x9c\x9f\xe5\x8c\x96\r\nG28\r\n", b"G28", b"G28\r", b""):
            with self.subTest(base=base):
                self.base_file.write_bytes(base)
                separator = b"\n" if base and not base.endswith((b"\r", b"\n")) else b""
                expected = base + separator + module.generate_gcode(self.config, [(0, 0)]).encode("utf-8")
                output_path = self.base_file.parent / "pull.gcode"
                args = ["--pins", "0,0", "--base-file", str(self.base_file)]
                self.assertEqual(module.main([*args, "--output", str(output_path)]), 0)
                self.assertEqual(output_path.read_bytes(), expected)
                buffer = io.BytesIO()
                stdout = io.TextIOWrapper(buffer, encoding="utf-8")
                with redirect_stdout(stdout):
                    self.assertEqual(module.main(args), 0)
                self.assertEqual(buffer.getvalue(), expected)
                self.assertEqual(self.base_file.read_bytes(), base)

    def test_default_base_file_is_used(self):
        output_path = self.base_file.parent / "pull.gcode"
        with patch.object(module, "DEFAULT_BASE_FILE", self.base_file):
            self.assertEqual(module.main(["--pins", "0,0", "--output", str(output_path)]), 0)
        self.assertTrue(output_path.read_bytes().startswith(self.base_file.read_bytes()))

    def test_missing_base_does_not_modify_existing_output(self):
        output_path = self.base_file.parent / "pull.gcode"
        output_path.write_bytes(b"existing output")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(module.main(["--pins", "0,0", "--base-file", str(self.base_file.parent / "missing.gcode"),
                                          "--output", str(output_path)]), 1)
        self.assertEqual(output_path.read_bytes(), b"existing output")

    def test_base_cannot_be_overwritten_by_output(self):
        original = self.base_file.read_bytes()
        with redirect_stderr(io.StringIO()):
            self.assertEqual(module.main(["--pins", "0,0", "--base-file", str(self.base_file),
                                          "--output", str(self.base_file)]), 1)
        self.assertEqual(self.base_file.read_bytes(), original)

    def test_invalid_settings_do_not_write_output(self):
        for field, value in [("legacy", None), ("type", "typo"), ("parity", 2), ("parity", True),
                             ("x_hook", float("nan")), ("feed_rate", 0)]:
            config = copy.deepcopy(self.config)
            if field == "legacy":
                config["pull_gcode"].pop("alternating")
            elif field == "type":
                config["pull_gcode"]["type"] = value
            elif field == "parity":
                config["pull_gcode"]["alternating"]["row_parity"] = value
            else:
                config["pull_gcode"]["normal"][field] = value
            with self.subTest(field=field, value=value), tempfile.TemporaryDirectory() as temp:
                config_path = Path(temp) / "config.json"
                output_path = Path(temp) / "pull.gcode"
                config_path.write_text(json.dumps(config), encoding="utf-8")
                with redirect_stderr(io.StringIO()):
                    self.assertEqual(module.main(["--config", str(config_path), "--pins", "0,0",
                                                  "--base-file", str(self.base_file),
                                                  "--output", str(output_path)]), 1)
                self.assertFalse(output_path.exists())


if __name__ == "__main__":
    unittest.main()

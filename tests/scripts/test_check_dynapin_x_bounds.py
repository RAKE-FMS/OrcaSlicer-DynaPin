import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/check_dynapin_x_bounds.py"
SPEC = importlib.util.spec_from_file_location("check_dynapin_x_bounds", SCRIPT)
check = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check
SPEC.loader.exec_module(check)


def segment(line, x0, x1, y=0., z=.3, height=.3, landing=False, role="Support", width=.4):
    return check.Segment(line, (x0, y), (x1, y), z, height, width, role, landing)


class ConnectedBoundsTests(unittest.TestCase):
    def test_tracks_four_connected_layers_not_global_layer_events(self):
        segments = [segment(1, 100, 110, landing=True)]
        for i in range(1, 6):
            segments.append(segment(10 + i, 100, 110, z=.3 * (i + 1), role="Outer wall"))
            # Disconnected geometry interleaved in Z must not count or fail.
            segments.append(segment(20 + i, 10, 20, y=20, z=.3 * i + .1, height=.1))
        rows, bad = check.analyze(segments, 4, 60, 165)
        self.assertEqual([r["layer"] for r in rows], [1, 2, 3, 4])
        self.assertEqual([round(r["z"], 6) for r in rows], [.3, .6, .9, 1.2])
        self.assertFalse(bad)

    def test_whole_laterally_connected_component_is_checked(self):
        segments = [segment(1, 100, 110, landing=True), segment(2, 109, 170)]
        rows, bad = check.analyze(segments, 4, 60, 165)
        self.assertEqual(rows[0]["segments"], 2)
        self.assertEqual([s["line"] for s in bad], [2])

    def test_width_and_start_endpoint_outside_are_failures(self):
        for x0, x1 in [(60, 100), (100, 165), (59, 100)]:
            with self.subTest(x0=x0, x1=x1):
                _, bad = check.analyze([segment(1, x0, x1, landing=True)], 4, 60, 165)
                self.assertEqual(len(bad), 1)
        _, bad = check.analyze([segment(1, 60.2, 164.8, landing=True)], 4, 60, 165)
        self.assertFalse(bad)

    def test_z_gap_does_not_connect_and_crossing_lines_do(self):
        a = segment(1, 100, 110, landing=True)
        b = check.Segment(2, (105, -5), (105, 5), .6, .3, .4, "Support")
        c = segment(3, 100, 170, z=1.2)  # .3 mm air gap above b
        rows, bad = check.analyze([a, b, c], 4, 60, 165)
        self.assertEqual([r["layer"] for r in rows], [1, 2])
        self.assertFalse(bad)

    def test_collinear_disjoint_and_parallel_capsules(self):
        a = segment(1, 100, 110)
        self.assertFalse(check.touches(a, segment(2, 120, 130)))
        self.assertTrue(check.touches(a, segment(2, 100, 110, y=.4)))
        self.assertFalse(check.touches(a, segment(2, 100, 110, y=.5)))

    def test_discovery_excludes_min_py_but_keeps_mini_and_old(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("part.gcode", "min_part.gcode", "py_pull_part.gcode", "mini_part.gcode", "min.gcode", "old/part.GCODE"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            self.assertEqual({p.name for p in check.discover(root)}, {"part.gcode", "mini_part.gcode", "part.GCODE"})

    def test_parser_modal_state_pin_moves_and_positive_extrusion_only(self):
        source = """G90
M82
G92 X100 Y0 Z0 E0
;LAYER_CHANGE
;Z:0
;HEIGHT:0.3
; BEGIN_DYNAPIN_PULL ROW=0 COL=0
G1 X172
G1 X20
; END_DYNAPIN_PULL
; BEGIN_DYNAPIN_RETURN
G1 X100
; END_DYNAPIN_RETURN
G1 Z0.3
;TYPE:Support
;WIDTH:0.4
;_DYNAPIN_LANDING_FAN_START
G1 X110 E1
G1 X120 E1
G1 E0.5
G1 X110 E0.5
;_DYNAPIN_LANDING_FAN_END
G92 E0
G1 X100 E1
G91
M83
G1 X5 E1
"""
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "part.gcode"
            path.write_text(source)
            segments, pulls, _ = check.parse_gcode(path, 4)
        self.assertEqual(len(pulls), 1)
        self.assertEqual([(s.a[0], s.b[0], s.landing) for s in segments],
                         [(100, 110, True), (110, 100, False), (100, 105, False)])

    def test_no_marker_and_missing_four_layers_are_not_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "part.gcode"
            path.write_text(";HEIGHT:0.3\n;Z:0\n; BEGIN_DYNAPIN_PULL ROW=0 COL=0\n; END_DYNAPIN_PULL\n")
            self.assertEqual(check.check_file(path)["status"], "UNKNOWN")
            path.write_text("G90\nM83\n;HEIGHT:0.3\n;Z:0\n; BEGIN_DYNAPIN_PULL ROW=0 COL=0\n"
                            "; END_DYNAPIN_PULL\nG1 X100 Y0 Z0.3\n;WIDTH:0.4\n;TYPE:Support\n"
                            ";_DYNAPIN_LANDING_FAN_START\nG1 X110 E1\n;_DYNAPIN_LANDING_FAN_END\n")
            self.assertEqual(check.check_file(path)["status"], "UNKNOWN")
            path.write_text("; no pin pulls\n")
            self.assertEqual(check.check_file(path)["status"], "NO_PINS")


class MotionBoundsTests(unittest.TestCase):
    def scan(self, source):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "moves.gcode"
            path.write_text("G90\nG28\n" + source)
            return check.check_motion_x(path)

    def test_travel_at_exact_boundary_is_forbidden_without_any_pins(self):
        result = self.scan("G1 X167.999\nG1 X168\nG1 Y10\nG1 X100\n")
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(len(result["violations"]), 3)
        self.assertEqual(result["max_x"], 168.)

    def test_only_pull_block_is_exempt_return_is_checked(self):
        result = self.scan("; BEGIN_DYNAPIN_PULL ROW=0 COL=0\nG1 X172\nG1 X20\n"
                           "; END_DYNAPIN_PULL\n; BEGIN_DYNAPIN_RETURN\nG1 X169\n"
                           "; END_DYNAPIN_RETURN\nG1 X100\n")
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual([v["max_x"] for v in result["violations"]], [169., 169.])

    def test_exit_from_forbidden_region_after_pull_is_also_forbidden(self):
        result = self.scan("; BEGIN_DYNAPIN_PULL ROW=0 COL=0\nG1 X172\n"
                           "; END_DYNAPIN_PULL\nG1 X100\n")
        self.assertEqual(result["violations"][0]["x_start"], 172.)

    def test_relative_coordinates_and_g92_keep_machine_coordinates(self):
        result = self.scan("G1 X160\nG92 X0\nG1 X8\nG91\nG1 X-1\nG1 X2\n")
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["max_x"], 169.)
        self.assertEqual(len(result["violations"]), 3)

    def test_safe_pull_with_safe_return_and_no_xyz_extrusion(self):
        result = self.scan("G1 X100\n; BEGIN_DYNAPIN_PULL ROW=0 COL=0\n"
                           "G1 X172\nG1 E1\nG1 X20\n; END_DYNAPIN_PULL\n"
                           "; BEGIN_DYNAPIN_RETURN\nG1 X167.999\n; END_DYNAPIN_RETURN\nG1 E1\n")
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["max_x"], 167.999)

    def test_unsupported_arcs_and_broken_exemption_do_not_pass(self):
        for source in ("G2 X100 I50 J0\n", "; BEGIN_DYNAPIN_PULL ROW=0 COL=0\nG1 X172\n",
                       "; END_DYNAPIN_PULL\n"):
            with self.subTest(source=source):
                self.assertEqual(self.scan(source)["status"], "ERROR")


if __name__ == "__main__":
    unittest.main()

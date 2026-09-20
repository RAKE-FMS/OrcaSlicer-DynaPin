import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "slice_models_by_filament.py"
spec = importlib.util.spec_from_file_location("slice_models_by_filament", SCRIPT_PATH)
batch = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = batch
spec.loader.exec_module(batch)


class BatchSliceTests(unittest.TestCase):
    def test_recursive_discovery(self):
        with tempfile.TemporaryDirectory() as temp:
            models = Path(temp)
            for relative in ("root.3mf", "part/PLA.3mf", "deep/part/ABS.3MF"):
                path = models / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (models / "ignore.txt").touch()
            self.assertEqual(
                [models / "deep/part/ABS.3MF", models / "part/PLA.3mf", models / "root.3mf"],
                batch.discover_models(models),
            )

    def test_build_slice_command_uses_embedded_settings(self):
        command = batch.build_slice_command(
            Path("orca-slicer"), Path("models/part one/PLA Basic.3mf"), Path("temp/output")
        )
        self.assertEqual(
            ["orca-slicer", "--slice", "0", "--outputdir", "temp/output", "models/part one/PLA Basic.3mf"],
            command,
        )
        self.assertNotIn("--load-filaments", command)

    def test_resolve_slicer_prefers_repository_release(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            release = root / "build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer"
            release.parent.mkdir(parents=True)
            release.touch()
            with patch.object(batch.shutil, "which", return_value="/usr/bin/OrcaSlicer"):
                self.assertEqual(release, batch.resolve_slicer(repo_root=root, platform_name="darwin"))

    def test_publish_gcode_replaces_final(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "plate_1.gcode"
            destination = root / "outputs/part/ABS.gcode"
            source.write_text("new", encoding="utf-8")
            batch.publish_gcode(source, destination)
            self.assertEqual("new", destination.read_text(encoding="utf-8"))

    def test_slice_one_preserves_existing_output_on_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            model = root / "part.3mf"
            model.touch()
            final = root / "outputs/part.gcode"
            final.parent.mkdir()
            final.write_text("old", encoding="utf-8")

            def failed(command, **kwargs):
                return SimpleNamespace(returncode=17, stdout="invalid printer settings")

            def multiple_plates(command, **kwargs):
                output = Path(command[command.index("--outputdir") + 1])
                (output / "plate_1.gcode").write_text("one", encoding="utf-8")
                (output / "plate_2.gcode").write_text("two", encoding="utf-8")
                return SimpleNamespace(returncode=0, stdout="")

            for runner in (failed, multiple_plates):
                result = batch.slice_one(Path("orca-slicer"), model, final, runner)
                self.assertFalse(result.success)
                self.assertEqual("old", final.read_text(encoding="utf-8"))

    def test_run_batch_preserves_relative_paths_and_runs_once_per_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            models = root / "models"
            for relative in ("root.3mf", "part/PLA.3mf", "deep/part/ABS.3mf"):
                path = models / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            commands = []

            def fake_run(command, **kwargs):
                commands.append(command)
                output = Path(command[command.index("--outputdir") + 1])
                (output / "plate_1.gcode").write_text("ok", encoding="utf-8")
                return SimpleNamespace(returncode=0, stdout="")

            results = batch.run_batch(models, root / "outputs", Path("orca-slicer"), fake_run)
            self.assertEqual(3, len(results))
            self.assertTrue(all(result.success for result in results))
            self.assertEqual(3, len(commands))
            self.assertTrue(all("--load-filaments" not in command for command in commands))
            for relative in ("root.gcode", "part/PLA.gcode", "deep/part/ABS.gcode"):
                self.assertTrue((root / "outputs" / relative).is_file())

    def test_main_uses_outputs_sibling_to_models(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            model = root / "models/part/PLA.3mf"
            model.parent.mkdir(parents=True)
            model.touch()
            slicer = root / "orca-slicer"
            slicer.touch()
            seen = []

            def fake_batch(models, outputs, executable):
                seen.append((models, outputs, executable))
                return [batch.SliceResult(True, model)]

            with patch.object(batch, "resolve_slicer", return_value=slicer), patch.object(batch, "run_batch", side_effect=fake_batch):
                self.assertEqual(0, batch.main(["--repo-root", str(root)]))
            self.assertEqual([(root.resolve() / "models", root.resolve() / "outputs", slicer)], seen)


if __name__ == "__main__":
    unittest.main()

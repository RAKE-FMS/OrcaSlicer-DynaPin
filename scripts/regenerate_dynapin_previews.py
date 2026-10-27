#!/usr/bin/env python3
"""Regenerate DynaPin normal-support previews without writing to models/."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
from zipfile import ZipFile

from slice_models_by_filament import resolve_slicer


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--slicer')
    parser.add_argument('--condition', choices=('default', 'dynapin'), default='dynapin')
    parser.add_argument('--white-background', action='store_true')
    parser.add_argument('--exclude-model', action='append', default=[], help='Model folder to skip')
    parser.add_argument('--output-dir', type=Path, default=root / 'outputs/dynapin-blockers')
    args = parser.parse_args()
    source_root = root / 'models'
    output = args.output_dir.resolve()
    if output == source_root or source_root in output.parents:
        parser.error('Output must be outside models/')
    slicer = resolve_slicer(args.slicer, root).resolve()
    sources = sorted(source_root.glob(f'*/{args.condition}/normal/*.3mf'))
    manifest = []
    failures = []
    for source in sources:
        if 'old' in (part.casefold() for part in source.relative_to(source_root).parts):
            continue
        slug = source.relative_to(source_root).parts[0]
        if slug in args.exclude_model:
            continue
        destination = output / slug / 'normal'
        destination.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        gcodes = sorted(path for path in source.parent.glob('*.gcode')
                        if not path.name.startswith(('min_', 'py_pull_')))
        if len(gcodes) != 1:
            raise ValueError(f'Expected one original G-code beside {source}')
        pins = list(dict.fromkeys(re.findall(r'^; BEGIN_DYNAPIN_PULL ROW=(\d+) COL=(\d+)',
                                             gcodes[0].read_text(), re.MULTILINE)))
        # Retain the saved model placement and reuse pins from the existing slice.
        project = destination / source.name
        with ZipFile(source) as original, ZipFile(project, 'w') as copied:
            for entry in original.infolist():
                data = original.read(entry.filename)
                if entry.filename == 'Metadata/project_settings.config':
                    settings = json.loads(data)
                    settings['enable_dynapin_placement_optimization'] = '0'
                    settings['dynapin_selected_pins'] = ' '.join(f'{row},{col}' for row, col in pins)
                    data = json.dumps(settings).encode()
                if entry.filename == 'Metadata/orca_preview_capture.json':
                    capture = json.loads(data)
                    for plate in capture['plates']:
                        plate['capture']['dynapin'] = args.condition == 'dynapin'
                        plate['capture']['dark'] = False
                        plate['capture']['background'] = ([1.0, 1.0, 1.0, 1.0] if args.white_background
                                                          else [231 / 255, 231 / 255, 231 / 255, 1.0])
                    data = json.dumps(capture).encode()
                copied.writestr(entry, data)
        print(f'Generating {slug}', flush=True)
        with (destination / 'slice.log').open('w') as log:
            result = subprocess.run([str(slicer), '--slice', '0', '--outputdir', str(destination), str(project)],
                                    stdout=log, stderr=subprocess.STDOUT)
        image = destination / 'plate_1.sliced.png'
        success = result.returncode == 0 and image.is_file()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, f'Source changed: {source}'
        manifest.append({'source': str(source.relative_to(root)), 'sha256': digest, 'success': success,
                         'image': str(image), 'slicer': str(slicer), 'placement_optimization': False,
                         'pins': pins, 'pin_source': str(gcodes[0].relative_to(root))})
        if not success:
            failures.append(slug)
        print(f'{"OK" if success else "FAILED"}: {slug}', flush=True)
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Completed {len(manifest) - len(failures)}/{len(manifest)}; failures: {failures}', flush=True)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())

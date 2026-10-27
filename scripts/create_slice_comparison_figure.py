#!/usr/bin/env python3
"""Create a PNG comparison sheet for original, default-support, and DynaPin views.

Expected real screenshot layout:

    models/<model>/default/normal/<model>.unsliced.png
    models/<model>/default/normal/<model>.sliced.png
    models/<model>/dynapin/normal/<model>*.sliced.png

The original model image is read from ``before.png`` or ``original.png`` in the
model directory, falling back to ``Metadata/plate_1.png`` in its 3MF file.
Each model is drawn as a card: original image centered at the top, Normal at
the lower left, and Tree at the lower right. ``--mock`` draws clearly marked
dummy support views for layout review when real slice screenshots are absent.

Examples:
    python3 scripts/create_slice_comparison_figure.py --mock
    python3 scripts/create_slice_comparison_figure.py
    python3 scripts/create_slice_comparison_figure.py --normal-dir Normal --tree-dir Tree
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Iterable
from zipfile import BadZipFile, ZipFile

from PIL import Image, ImageDraw, ImageFont, ImageOps


MODELS = (
    ("Angled_Cylinder", "Angled Cylinder"),
    ("Arch_Bridge", "Arch Bridge"),
    ("Simple_Bridge", "Simple Bridge"),
    ("3D_Fiducial_Marker", "Fiducial Marker"),
    ("Nefertiti", "Nefertiti"),
    ("Overhang_Angle_Bracket", "Angle Bracket"),
    ("RemoveGymnastBase", "Gymnast"),
    ("Rocker_Arm", "Rocker Arm"),
    ("Cheburashka", "Cheburashka"),
    ("Beast", "Beast"),
    ("Cow", "Cow"),
    ("bike-helmet", "Bike Helmet"),
    ("Enterprise", "Enterprise"),
    ("Stanford_Bunny", "Stanford Bunny"),
    ("Armadillo", "Armadillo"),
)
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp"}
ImageSource = Path | bytes | Image.Image


@dataclass(frozen=True)
class ModelImages:
    slug: str
    label: str
    original: ImageSource
    normal: Path | None = None
    tree: Path | None = None


def find_direct_image(model_dir: Path, model_slug: str) -> Path | None:
    """Find a pre-slice image explicitly placed in the model directory."""
    images = {
        path.stem.casefold(): path
        for path in model_dir.iterdir()
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    }
    for stem in ("before", "original", "model", model_slug.casefold()):
        if stem in images:
            return images[stem]
    return None


def find_condition_image(directory: Path, model_slug: str) -> Path | None:
    """Find a screenshot in a Normal/Tree folder, excluding the 3MF archive."""
    if not directory.is_dir():
        return None
    images = sorted(
        path for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
    )
    sliced_images = [path for path in images if path.name.endswith(".sliced.png")]
    if len(sliced_images) == 1:
        return sliced_images[0]
    preferred_stems = (model_slug, f"{model_slug}_slice", "slice", "sliced", "preview", "screenshot")
    for stem in preferred_stems:
        match = next((path for path in images if path.stem.casefold() == stem.casefold()), None)
        if match:
            return match
    return images[0] if len(images) == 1 else None


def load_3mf_preview(model_dir: Path, condition_dir: str, model_slug: str) -> bytes | None:
    """Return the plate thumbnail from a model's 3MF archive, if available."""
    archive_path = model_dir / condition_dir / f"{model_slug}.3mf"
    if not archive_path.is_file():
        return None
    try:
        with ZipFile(archive_path) as archive:
            return archive.read("Metadata/plate_1.png")
    except (OSError, BadZipFile, KeyError):
        return None


def find_original(model_dir: Path, model_slug: str) -> ImageSource | None:
    direct_image = find_direct_image(model_dir, model_slug)
    if direct_image:
        return direct_image
    originals = sorted((model_dir / "default/normal").glob("*.unsliced.png"))
    if len(originals) == 1:
        return originals[0]
    return load_3mf_preview(model_dir, "Normal", model_slug) or load_3mf_preview(model_dir, "Tree", model_slug)


def collect_models(
    models_dir: Path,
    normal_dir_name: str,
    tree_dir_name: str,
    mock: bool,
) -> list[ModelImages]:
    model_images = []
    for slug, label in MODELS:
        model_dir = models_dir / slug
        if not model_dir.is_dir():
            print(f"Skip {label}: model directory not found ({model_dir})", file=sys.stderr)
            continue

        original = find_original(model_dir, slug)
        if original is None:
            print(f"Skip {label}: no original image or embedded 3MF preview found", file=sys.stderr)
            continue

        normal = find_condition_image(model_dir / normal_dir_name, slug)
        tree = find_condition_image(model_dir / tree_dir_name, slug)
        if not mock and (normal is None or tree is None):
            missing = []
            if normal is None:
                missing.append(normal_dir_name)
            if tree is None:
                missing.append(tree_dir_name)
            print(f"Skip {label}: missing slice image(s) in {', '.join(missing)}", file=sys.stderr)
            continue
        model_images.append(ModelImages(slug, label, original, normal, tree))
    return model_images


def load_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = (
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica Neue.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size=size)
        except OSError:
            pass
    return ImageFont.load_default()


def open_image(source: ImageSource) -> Image.Image:
    if isinstance(source, Image.Image):
        return source.convert("RGBA")
    image_file = BytesIO(source) if isinstance(source, bytes) else source
    with Image.open(image_file) as image:
        return image.convert("RGBA")


def draw_fitted(
    canvas: Image.Image,
    source: ImageSource,
    box: tuple[int, int, int, int],
    background: tuple[int, int, int],
) -> None:
    left, top, right, bottom = box
    panel = Image.new("RGB", (right - left, bottom - top), background)
    source_image = open_image(source)
    fitted = ImageOps.fit(source_image, panel.size, method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    x = (panel.width - fitted.width) // 2
    y = (panel.height - fitted.height) // 2
    panel.paste(fitted, (x, y), fitted.getchannel("A"))
    canvas.paste(panel, (left, top))


def draw_mock_slice(kind: str, size: tuple[int, int]) -> Image.Image:
    """Draw a generic, explicitly fictional slicer viewport for mock mode."""
    width, height = size
    image = Image.new("RGB", size, (226, 229, 232))
    draw = ImageDraw.Draw(image)

    plate_top = int(height * 0.38)
    draw.polygon(
        [(0, plate_top + 10), (width * 0.56, plate_top - 18), (width, plate_top + 4), (width, height), (0, height)],
        fill=(48, 54, 58),
    )
    grid = (75, 82, 86)
    for index in range(1, 8):
        x = int(width * index / 8)
        draw.line((x, plate_top + 6, x, height), fill=grid, width=1)
    for index in range(1, 5):
        y = plate_top + int((height - plate_top) * index / 5)
        draw.line((0, y, width, y), fill=grid, width=1)

    green = (24, 190, 45)
    green_dark = (13, 135, 34)
    orange = (222, 113, 30)
    orange_dark = (167, 69, 24)
    support_contacts = [
        (int(width * ratio), int(height * y_ratio))
        for ratio, y_ratio in ((0.28, 0.43), (0.40, 0.47), (0.58, 0.43), (0.73, 0.39))
    ]
    bed_y = int(height * 0.81)

    if kind == "normal":
        for x, contact_y in support_contacts:
            pillar_width = max(6, width // 32)
            draw.rectangle((x - pillar_width, contact_y, x + pillar_width, bed_y), fill=green)
            draw.line((x + pillar_width, contact_y, x + pillar_width, bed_y), fill=green_dark, width=2)
            draw.rectangle((x - pillar_width * 2, bed_y - 2, x + pillar_width * 2, bed_y + 5), fill=green_dark)
    else:
        trunk_x = int(width * 0.51)
        trunk_top = int(height * 0.47)
        draw.line((trunk_x, bed_y, trunk_x, trunk_top), fill=green_dark, width=max(12, width // 15))
        for x, contact_y in support_contacts:
            draw.line((trunk_x, int(height * 0.65), x, contact_y), fill=green, width=max(7, width // 24))
            draw.ellipse((x - 5, contact_y - 4, x + 5, contact_y + 5), fill=green)
        draw.rectangle((trunk_x - 12, bed_y - 2, trunk_x + 12, bed_y + 5), fill=green_dark)

    # A floating orange bridge/platform represents the same generic overhang in both views.
    object_points = [
        (int(width * 0.20), int(height * 0.29)),
        (int(width * 0.68), int(height * 0.22)),
        (int(width * 0.84), int(height * 0.34)),
        (int(width * 0.33), int(height * 0.43)),
    ]
    depth = max(6, height // 18)
    draw.polygon([(x, y + depth) for x, y in object_points], fill=orange_dark)
    draw.polygon(object_points, fill=orange, outline=(121, 66, 28))
    draw.line(object_points + [object_points[0]], fill=(247, 155, 70), width=2)

    tag_font = load_font(max(11, height // 13), bold=True)
    tag = "MOCK"
    tag_box = draw.textbbox((0, 0), tag, font=tag_font)
    draw.rounded_rectangle((6, 5, tag_box[2] + 18, tag_box[3] + 13), radius=4, fill=(255, 255, 255))
    draw.text((12, 8), tag, fill=(65, 69, 72), font=tag_font)
    return image


def fit_font(draw: ImageDraw.ImageDraw, text: str, max_width: int, size: int, bold: bool = True) -> ImageFont.ImageFont:
    for candidate_size in range(size, 11, -1):
        font = load_font(candidate_size, bold)
        if draw.textbbox((0, 0), text, font=font)[2] <= max_width:
            return font
    return load_font(12, bold)


def draw_model_card(
    canvas: Image.Image,
    model: ModelImages,
    x: int,
    y: int,
    card_width: int,
    card_height: int,
    mock: bool,
) -> None:
    draw = ImageDraw.Draw(canvas)

    inner_margin = 13
    panel_gap = 10
    panel_width = ((card_width - 2 * inner_margin - panel_gap) // 2) // 4 * 4
    panel_height = panel_width * 3 // 4
    original_width, original_height = panel_width, panel_height
    original_left = x + (card_width - original_width) // 2
    original_top = y + 24
    draw_fitted(
        canvas,
        model.original,
        (original_left, original_top, original_left + original_width, original_top + original_height),
        (255, 255, 255),
    )

    panels_top = original_top + original_height + panel_gap
    for index, (kind, image_source) in enumerate(
        (
            ("normal", model.normal),
            ("tree", model.tree),
        )
    ):
        left = x + inner_margin + index * (panel_width + panel_gap)
        if mock:
            assert kind in ("normal", "tree")
            image_source = draw_mock_slice(kind, (panel_width, panel_height))
        if image_source is None:
            continue
        draw_fitted(
            canvas,
            image_source,
            (left, panels_top, left + panel_width, panels_top + panel_height),
            (255, 255, 255),
        )

    title_font = fit_font(draw, model.label, card_width - 20, 24)
    title_width = draw.textbbox((0, 0), model.label, font=title_font)[2]
    title_y = panels_top + panel_height + 4
    draw.text((x + (card_width - title_width) / 2, title_y), model.label, fill=(45, 48, 51), font=title_font)


def make_figure(
    models: Iterable[ModelImages],
    columns: int,
    card_width: int,
    card_height: int,
    gap_x: int,
    gap_y: int,
    page_margin: int,
    mock: bool,
) -> Image.Image:
    models = list(models)
    rows = (len(models) + columns - 1) // columns
    width = page_margin * 2 + columns * card_width + (columns - 1) * gap_x
    height = page_margin * 2 + rows * card_height + (rows - 1) * gap_y
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    for index, model in enumerate(models):
        row, column = divmod(index, columns)
        x = page_margin + column * (card_width + gap_x)
        y = page_margin + row * (card_height + gap_y)
        draw_model_card(canvas, model, x, y, card_width, card_height, mock)
    return canvas


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models-dir", type=Path, default=repo_root / "models", help="Directory containing the model folders")
    parser.add_argument("--normal-dir", default="default/normal", help="Folder containing default-support slice images")
    parser.add_argument("--tree-dir", default="dynapin/normal", help="Folder containing DynaPin slice images")
    parser.add_argument("--output", type=Path, help="Output PNG path")
    parser.add_argument("--columns", type=int, default=5, help="Number of model cards per row")
    parser.add_argument("--mock", action="store_true", help="Draw fictional Normal/Tree views for a layout mock")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.columns < 1:
        print("columns must be positive", file=sys.stderr)
        return 2
    if not args.models_dir.is_dir():
        print(f"Models directory does not exist: {args.models_dir}", file=sys.stderr)
        return 2

    models = collect_models(args.models_dir, args.normal_dir, args.tree_dir, args.mock)
    if not models:
        print("No models with original images were found.", file=sys.stderr)
        return 1

    output = args.output
    if output is None:
        filename = "slice-comparison-mock.png" if args.mock else "slice-comparison.png"
        output = Path(__file__).resolve().parents[1] / "outputs/figures" / filename

    figure = make_figure(
        models,
        columns=args.columns,
        card_width=350,
        card_height=326,
        gap_x=14,
        gap_y=18,
        page_margin=28,
        mock=args.mock,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.save(output, format="PNG", optimize=True)
    print(f"Saved {len(models)} model cards to {output} ({figure.width}x{figure.height})")
    if args.mock:
        print("Normal and Tree panels are fictional layout placeholders marked MOCK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

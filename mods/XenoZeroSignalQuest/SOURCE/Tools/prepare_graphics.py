from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps


def find_workspace_root() -> Path:
    configured = os.environ.get("SRHD_WORKSPACE_ROOT")
    if configured:
        return Path(configured).expanduser().resolve()
    for parent in Path(__file__).resolve().parents:
        if (parent / "Tools" / "SRHDModKit" / "srhd.py").is_file():
            return parent
    raise RuntimeError("SRHD workspace root was not found; set SRHD_WORKSPACE_ROOT")


WORKSPACE_ROOT = find_workspace_root()
MOD_ROOT = Path(__file__).resolve().parents[2]
ASSET_ROOT = Path(
    os.environ.get(
        "SRHD_XZS_ASSET_ROOT",
        WORKSPACE_ROOT / "Assets" / "XenoZeroSignalQuest" / "SourceImages",
    )
)
GENERATED_ROOT = MOD_ROOT / "SOURCE" / "Graphics" / "GeneratedRaw"
PREPARED_ROOT = MOD_ROOT / "SOURCE" / "Graphics" / "ExistingPrepared"
GAME_ROOT = MOD_ROOT / "DATA" / "PQI"
REPORT_ROOT = Path(
    os.environ.get(
        "SRHD_XZS_REPORT_ROOT",
        WORKSPACE_ROOT / "Reports" / "XenoZeroSignalQuest" / "v1.4.0",
    )
)

TARGET_SIZE = (343, 394)

# Индексы относятся к стабильной сортировке 26 исходников в пользовательской
# папке. Первые семнадцать выбранных файлов имеют встроенную золотую/синюю
# рамку; поздние девять — чистые иллюстрации без неё. В версии 1.1 возвращены
# четыре ранее неиспользованных кадра коридоров и диспетчерской.
EXISTING_SELECTION = {
    "XZS_00": (1, True, 0.46),
    "XZS_01": (2, True, 0.45),
    "XZS_02": (3, True, 0.46),
    "XZS_03": (4, True, 0.46),
    "XZS_04": (5, True, 0.50),
    "XZS_05": (26, False, 0.48),
    "XZS_06": (6, True, 0.48),
    "XZS_08": (8, True, 0.48),
    "XZS_10": (10, True, 0.47),
    "XZS_11": (11, True, 0.48),
    "XZS_12": (12, True, 0.47),
    "XZS_13": (13, True, 0.46),
    "XZS_14": (14, True, 0.47),
    "XZS_15": (15, True, 0.47),
    "XZS_16": (16, True, 0.47),
    "XZS_17": (17, True, 0.47),
    "XZS_18": (21, False, 0.50),
    "XZS_19": (22, False, 0.48),
    "XZS_20": (24, False, 0.48),
    "XZS_21": (25, False, 0.48),
    "XZS_30": (18, False, 0.48),
    "XZS_31": (19, False, 0.48),
    "XZS_32": (20, False, 0.48),
    "XZS_33": (23, False, 0.48),
}

GENERATED_NUMBERS = range(22, 29)
GENERATED_V11_NUMBERS = range(34, 46)
GENERATED_V12_NUMBERS = range(46, 64)
GENERATED_V13_NUMBERS = range(64, 71)
ASSET_NUMBERS = tuple(number for number in range(71) if number not in {7, 9, 29})
GENERATED_FOCUS_Y = {
    64: 0.42,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def portable_manifest_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(WORKSPACE_ROOT).as_posix()
    except ValueError:
        return f"external/{resolved.name}"


def remove_embedded_frame(image: Image.Image) -> Image.Image:
    width, height = image.size
    # Рамка занимает около 3.5–4% ширины и 3% высоты. Дополнительный небольшой
    # отступ гарантированно убирает золотую трубу и синюю подложку по краям.
    x_margin = max(42, round(width * 0.043))
    y_margin = max(46, round(height * 0.034))
    return image.crop((x_margin, y_margin, width - x_margin, height - y_margin))


def fit_game_view(image: Image.Image, focus_y: float) -> Image.Image:
    return ImageOps.fit(
        image.convert("RGB"),
        TARGET_SIZE,
        method=Image.Resampling.LANCZOS,
        centering=(0.5, focus_y),
    )


def export_asset(key: str, source: Path, *, framed: bool, focus_y: float) -> dict[str, object]:
    with Image.open(source) as opened:
        image = ImageOps.exif_transpose(opened).convert("RGB")
        source_size = image.size
        if framed:
            image = remove_embedded_frame(image)
        prepared = fit_game_view(image, focus_y)

    PREPARED_ROOT.mkdir(parents=True, exist_ok=True)
    GAME_ROOT.mkdir(parents=True, exist_ok=True)
    prepared_png = PREPARED_ROOT / f"{key}.png"
    game_jpg = GAME_ROOT / f"{key}.jpg"
    prepared.save(prepared_png, format="PNG", optimize=True)
    prepared.save(
        game_jpg,
        format="JPEG",
        quality=92,
        optimize=True,
        progressive=False,
        subsampling=0,
    )
    return {
        "key": key,
        "source": portable_manifest_path(source),
        "source_size": list(source_size),
        "framed_source": framed,
        "focus_y": focus_y,
        "output_size": list(TARGET_SIZE),
        "png_sha256": sha256(prepared_png),
        "jpg_sha256": sha256(game_jpg),
    }


def make_contact_sheet() -> Path:
    columns = 5
    rows = (len(ASSET_NUMBERS) + columns - 1) // columns
    label_height = 24
    sheet = Image.new("RGB", (columns * 343, rows * (394 + label_height)), (18, 22, 28))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for position, number in enumerate(ASSET_NUMBERS):
        key = f"XZS_{number:02d}"
        with Image.open(GAME_ROOT / f"{key}.jpg") as image:
            x = (position % columns) * 343
            y = (position // columns) * (394 + label_height)
            sheet.paste(image.convert("RGB"), (x, y))
            draw.rectangle((x, y + 394, x + 342, y + 417), fill=(18, 22, 28))
            draw.text((x + 8, y + 399), key, fill=(225, 232, 240), font=font)
    REPORT_ROOT.mkdir(parents=True, exist_ok=True)
    output = REPORT_ROOT / "XenoZeroSignalQuest_contact_sheet.jpg"
    sheet.save(output, format="JPEG", quality=90, optimize=True, subsampling=0)
    return output


def main() -> None:
    source_files = sorted(ASSET_ROOT.glob("*.png"), key=lambda item: item.name)
    if len(source_files) != 26:
        raise RuntimeError(f"Expected 26 source PNGs, found {len(source_files)}")

    records: list[dict[str, object]] = []
    for key, (one_based_index, framed, focus_y) in EXISTING_SELECTION.items():
        records.append(
            export_asset(
                key,
                source_files[one_based_index - 1],
                framed=framed,
                focus_y=focus_y,
            )
        )

    for number in (*GENERATED_NUMBERS, *GENERATED_V11_NUMBERS, *GENERATED_V12_NUMBERS, *GENERATED_V13_NUMBERS):
        key = f"XZS_{number:02d}"
        source = GENERATED_ROOT / f"{key}.png"
        if not source.is_file():
            raise FileNotFoundError(source)
        records.append(
            export_asset(
                key,
                source,
                framed=False,
                focus_y=GENERATED_FOCUS_Y.get(number, 0.48),
            )
        )

    expected_keys = {f"XZS_{i:02d}" for i in ASSET_NUMBERS}
    if len(records) != len(expected_keys) or {row["key"] for row in records} != expected_keys:
        raise RuntimeError("Asset set is incomplete or contains duplicate keys")

    contact_sheet = make_contact_sheet()
    manifest = {
        "schema": "xeno-zero-signal-graphics-v4",
        "target_size": list(TARGET_SIZE),
        "jpeg_quality": 92,
        "existing_sources_total": 26,
        "existing_sources_selected": len(EXISTING_SELECTION),
        "generated_sources": len(GENERATED_NUMBERS) + len(GENERATED_V11_NUMBERS) + len(GENERATED_V12_NUMBERS) + len(GENERATED_V13_NUMBERS),
        "contact_sheet": portable_manifest_path(contact_sheet),
        "assets": records,
    }
    output = MOD_ROOT / "SOURCE" / "Graphics" / "graphics-manifest.json"
    output.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(output), "contact_sheet": str(contact_sheet), "assets": len(records)}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

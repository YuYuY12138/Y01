"""独立读取交付物与官方配置，核对抽帧、合成、时长和来源哈希。"""
import configparser
from hashlib import sha256
import json
from pathlib import Path
from zipfile import ZipFile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main():
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    sources = json.loads((ROOT / "sources.json").read_text(encoding="utf-8"))
    assert manifest["ue_imported"] is False and manifest["ue_verified"] is False
    for item in sources["files"]:
        assert sha256((ROOT / item["file"]).read_bytes()).hexdigest() == item["sha256"], item["file"]
    with ZipFile(ROOT / "Original/isometric_hero.zip") as archive:
        assert archive.testzip() is None
        for item in sources["files"]:
            if "archive_member" in item:
                assert archive.read(item["archive_member"]) == (ROOT / item["file"]).read_bytes()
    layer_names = {"B": "steel_armor.png", "H": "male_head1.png", "R": "longsword.png", "L": "shield.png"}
    layers = {key: Image.open(ROOT / "Original/Hero" / name).convert("RGBA") for key, name in layer_names.items()}
    hero = Image.new("RGBA", (4096, 1024))
    for row, order in enumerate(("BRHL", "BRHL", "BLHR", "BLHR", "BLHR", "BLHR", "BRHL", "BRHL")):
        target = Image.new("RGBA", (4096, 128))
        for key in order:
            target.alpha_composite(layers[key].crop((0, row * 128, 4096, (row + 1) * 128)))
        hero.paste(target, (0, row * 128))
    assert hero.tobytes() == Image.open(ROOT / "Prepared/Hero/Hero_SteelSwordShield_Full.png").convert("RGBA").tobytes()
    skeleton = Image.open(ROOT / "Original/Skeleton/skeleton_0.png").convert("RGBA")
    assert sha256(skeleton.tobytes()).hexdigest() == "5825924d986e8b0c2aca5d597dbf9eee22e1913d43bd7405a72e3c9a53990231"
    image_sources = {"Hero": hero, "Skeleton": skeleton}
    configs = {}
    for actor in image_sources:
        config = configparser.ConfigParser()
        content = (ROOT / f"Evidence/{actor.lower()}_v014.txt").read_text(encoding="utf-8")
        config.read_string("[root]\n" + content)
        assert config.getint("root", "render_offset_x") == 64
        assert config.getint("root", "render_offset_y") == 96
        configs[actor] = config
    action_sections = {"Idle": "stance", "Move": "run", "Attack": "melee", "Hit": "hit", "Die": "die"}
    cells, empty, edge = 0, 0, 0
    for item in manifest["prepared"]:
        actor, action = item["actor"], item["action"]
        config = configs[actor]
        section = action_sections[action]
        start, count = config.getint(section, "position"), config.getint(section, "frames")
        assert item["source_columns_zero_based"] == list(range(start, start + count))
        assert item["source_release_fps"] is None
        assert item["preview_reference_ms_per_frame"] == config.getint(section, "duration")
        path = ROOT / item["file"]
        data = path.read_bytes()
        assert sha256(data).hexdigest() == item["file_sha256"]
        with Image.open(path) as output:
            assert output.mode == "RGBA" and output.size == (count * 128, 1024)
            for direction in range(8):
                for index in range(count):
                    actual = output.crop((index * 128, direction * 128, (index + 1) * 128, (direction + 1) * 128))
                    expected = image_sources[actor].crop(((start + index) * 128, direction * 128, (start + index + 1) * 128, (direction + 1) * 128))
                    assert actual.tobytes() == expected.tobytes()
                    bounds = actual.getchannel("A").getbbox()
                    empty += bounds is None
                    edge += bool(bounds and (bounds[0] == 0 or bounds[1] == 0 or bounds[2] == 128 or bounds[3] == 128))
                    cells += 1
    assert len(manifest["prepared"]) == 10 and cells == 384 and empty == 0 and edge == 6
    gif_report = []
    expected_durations = {"Idle": 2160, "Move": 480, "Attack": 960, "Hit": 720, "Die": 1320}
    for action, expected in expected_durations.items():
        path = ROOT / f"Preview/{action}_8Directions.gif"
        with Image.open(path) as image:
            assert image.size == (2112, 660) and image.info["loop"] == 0
            durations = []
            for index in range(image.n_frames):
                image.seek(index)
                image.load()
                durations.append(image.info["duration"])
            assert sum(durations) == expected, (action, durations)
            gif_report.append({"file": path.relative_to(ROOT).as_posix(), "encoded_frames": image.n_frames, "cycle_ms": sum(durations)})
    for name in ("Overview.png", "Hero_SE_FrameOrder.png", "Skeleton_SE_FrameOrder.png"):
        with Image.open(ROOT / "Preview" / name) as image:
            image.verify()
    assert not list(ROOT.rglob("*.uasset"))
    report = {"result": "PASS_WITH_SOURCE_EDGE_NOTES", "unit_tests_separate": "test_sample.py", "verified_source_records": len(sources["files"]),
        "action_sheets": 10, "frame_cells": cells, "pixel_mismatches": 0, "empty_frames": empty,
        "source_edge_touch_frames": edge, "gif_metadata": gif_report, "ue_imported": False, "ue_verified": False}
    (ROOT / "verification.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    inventory = []
    for path in sorted(ROOT.rglob("*")):
        if path.is_file() and path.name != "inventory.json" and "__pycache__" not in path.parts:
            data = path.read_bytes()
            inventory.append({"file": path.relative_to(ROOT).as_posix(), "bytes": len(data), "sha256": sha256(data).hexdigest()})
    (ROOT / "inventory.json").write_text(json.dumps(inventory, indent=2, ensure_ascii=False), encoding="utf-8")
    for item in json.loads((ROOT / "inventory.json").read_text(encoding="utf-8")):
        assert sha256((ROOT / item["file"]).read_bytes()).hexdigest() == item["sha256"]
    print(json.dumps(report, indent=2))
    print(f"INVENTORY PASS: {len(inventory)} files plus inventory.json")


if __name__ == "__main__":
    main()

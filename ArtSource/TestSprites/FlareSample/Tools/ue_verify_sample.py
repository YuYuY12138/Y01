"""在当前 UE 中只读核验指定动画资产；不导入、不改地图、不保存包。"""
import sys
sys.dont_write_bytecode = True
import json
import math
from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path
import unreal

SOURCE = Path(r"G:\GarbageProject\Y01\ArtSource\TestSprites\FlareSample")
ROOT = "/Game/TestArt/Sprites/FlareSample"
CONTENT = Path(r"G:\GarbageProject\Y01\Content")
DIRECTIONS = ("W", "NW", "N", "NE", "E", "SE", "S", "SW")
COUNTS = {"Idle": 4, "Move": 8, "Attack": 4, "Hit": 2, "Die": 6}
MATERIAL = "/Paper2D/TranslucentUnlitSpriteMaterial.TranslucentUnlitSpriteMaterial"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def verify():
    check(Path(unreal.Paths.get_project_file_path()).resolve() == Path(r"G:\GarbageProject\Y01\Y01.uproject").resolve(), "错误的工程")
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    expected = []
    for actor in ("Hero", "Skeleton"):
        for action, count in COUNTS.items():
            folder = f"{ROOT}/{actor}/{action}"
            expected.append(f"{folder}/T_{actor}_{action}")
            for direction in DIRECTIONS:
                expected.extend(f"{folder}/S_{actor}_{action}_{direction}_{index:02d}" for index in range(count))
                expected.append(f"{folder}/FB_{actor}_{action}_{direction}")
    missing = [path for path in expected if not assets.does_asset_exist(path)]
    check(not missing, f"预期474个新资产尚未齐全：缺{len(missing)}个；首项={missing[:1]}")
    actual = {str(path).split(".")[0] for path in assets.list_assets(ROOT, recursive=True, include_folder=False)}
    check(actual == set(expected), "限定目录出现非预期资产或缺失")
    material = assets.load_asset(MATERIAL)
    records = []
    for actor in ("Hero", "Skeleton"):
        for action, count in COUNTS.items():
            folder = f"{ROOT}/{actor}/{action}"
            texture = assets.load_asset(f"{folder}/T_{actor}_{action}")
            check(isinstance(texture, unreal.Texture2D), "纹理类型错误")
            check((texture.blueprint_get_size_x(), texture.blueprint_get_size_y()) == (128 * count, 1024), "纹理尺寸错误")
            check(texture.get_editor_property("filter") == unreal.TextureFilter.TF_NEAREST, "纹理过滤错误")
            check(texture.get_editor_property("mip_gen_settings") == unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS, "Mip设置错误")
            check(texture.get_editor_property("srgb") and not texture.get_editor_property("compression_no_alpha"), "纹理sRGB或Alpha错误")
            check(texture.get_editor_property("compression_settings") == unreal.TextureCompressionSettings.TC_EDITOR_ICON, "纹理压缩错误")
            milliseconds = {"Idle": 180 if actor == "Hero" else 120, "Move": 60, "Attack": 90, "Hit": 60, "Die": 120}[action]
            for row, direction in enumerate(DIRECTIONS):
                sprites = []
                for column in range(count):
                    path = f"{folder}/S_{actor}_{action}_{direction}_{column:02d}"
                    sprite = assets.load_asset(path)
                    check(isinstance(sprite, unreal.PaperSprite), "Sprite类型错误")
                    uv = sprite.get_editor_property("source_uv")
                    size = sprite.get_editor_property("source_dimension")
                    pivot = sprite.get_editor_property("custom_pivot_point")
                    check((uv.x, uv.y) == (column * 128, row * 128), path + " UV错误")
                    check((size.x, size.y) == (128, 128), path + " 尺寸错误")
                    check((pivot.x - uv.x, pivot.y - uv.y) == (64, 96), path + " 局部锚点错误")
                    check(sprite.get_editor_property("pivot_mode") == unreal.SpritePivotMode.CUSTOM, "锚点模式错误")
                    check(sprite.get_editor_property("snap_pivot_to_pixel_grid"), "像素吸附错误")
                    check(math.isclose(sprite.get_editor_property("pixels_per_unreal_unit"), 1.0), "PPU错误")
                    check(sprite.get_editor_property("source_texture") == texture, "源纹理错误")
                    check(sprite.get_editor_property("default_material") == material, "Sprite材质错误")
                    check(sprite.get_editor_property("sprite_collision_domain") == unreal.SpriteCollisionMode.NONE, "不应生成碰撞")
                    geometry = sprite.get_editor_property("render_geometry")
                    check(geometry.get_editor_property("geometry_type") == unreal.SpritePolygonMode.SOURCE_BOUNDING_BOX, "几何裁边错误")
                    shapes = geometry.get_editor_property("shapes")
                    check(len(shapes) == 1, "源矩形几何数量错误")
                    box = shapes[0].get_editor_property("box_size")
                    check((box.x, box.y) == (128, 128), "烘焙源矩形尺寸错误")
                    sprites.append(sprite)
                flipbook = assets.load_asset(f"{folder}/FB_{actor}_{action}_{direction}")
                check(isinstance(flipbook, unreal.PaperFlipbook), "Flipbook类型错误")
                order = [0, 1, 2, 3, 2, 1] if action == "Idle" else list(range(count))
                check(flipbook.get_num_frames() == len(order), "播放帧数错误")
                check(flipbook.get_num_key_frames() == len(order), "关键帧数错误")
                fps = flipbook.get_editor_property("frames_per_second")
                check(math.isclose(fps, 1000 / milliseconds, rel_tol=1e-6), "参考FPS错误")
                check(math.isclose(flipbook.get_total_duration(), len(order) * milliseconds / 1000, rel_tol=1e-6), "总时长错误")
                check(flipbook.get_editor_property("default_material") == material, "Flipbook材质错误")
                for index, source_index in enumerate(order):
                    check(flipbook.get_sprite_at_frame(index) == sprites[source_index], "播放帧序错误")
                check(flipbook.get_sprite_at_frame(len(order)) is None, "播放帧上界错误")
                records.append({"asset": flipbook.get_path_name(), "source_frames_per_direction": count, "key_order": order,
                                "reference_fps": fps, "duration_seconds": flipbook.get_total_duration(), "source_release_fps": None})
    disk_records = []
    for package in expected:
        path = CONTENT / (package.removeprefix("/Game/") + ".uasset")
        check(path.is_file() and path.stat().st_size > 0, "未保存到磁盘：" + package)
        disk_records.append({"asset": package, "file": str(path), "bytes": path.stat().st_size, "sha256": sha256(path.read_bytes()).hexdigest()})
    return {"result": "PASS", "utc": datetime.now(timezone.utc).isoformat(), "textures": 10, "sprites": 384, "flipbooks": 80,
            "frame_size": [128, 128], "local_pivot": [64, 96], "pixels_per_unreal_unit": 1.0, "material": MATERIAL,
            "asset_properties_verified_in_ue": True, "disk_files_verified": len(disk_records),
            "visual_playback_verified": False, "gameplay_connected": False, "flipbook_specs": records, "files": disk_records}


try:
    report = verify()
except Exception as error:
    report = {"result": "FAIL", "utc": datetime.now(timezone.utc).isoformat(), "error": str(error), "error_type": type(error).__name__}
    unreal.log_error("Y01_ART_VERIFY_FAIL " + str(error))
    (SOURCE / "UE_Verification_Last.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    raise
else:
    (SOURCE / "UE_Verification_Last.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    unreal.log("Y01_ART_VERIFY_PASS textures=10 sprites=384 flipbooks=80 disk=474")

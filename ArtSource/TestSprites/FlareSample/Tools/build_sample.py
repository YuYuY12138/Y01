"""生成离线美术小样；不访问 Source、Config、Content 或 UE。"""
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
DIRECTIONS = ("W", "NW", "N", "NE", "E", "SE", "S", "SW")
ACTIONS = ("Idle", "Move", "Attack", "Hit", "Die")
LAYER_FILES = {"body": "steel_armor.png", "head": "male_head1.png", "sword": "longsword.png", "shield": "shield.png"}
FONT_FILE = Path("C:/Windows/Fonts/msyh.ttc")


def layer_order(row):
    if row not in range(8):
        raise ValueError("方向行必须在 0 到 7 之间")
    if row in (0, 1, 6, 7):
        return ("body", "sword", "head", "shield")
    return ("body", "shield", "head", "sword")


def columns(actor, action):
    if actor not in ("Hero", "Skeleton"):
        raise ValueError(actor)
    hit_start = 18 if actor == "Hero" else 22
    ranges = {"Idle": (0, 4), "Move": (4, 8), "Attack": (12, 4), "Hit": (hit_start, 2), "Die": (hit_start, 6)}
    if action not in ranges:
        raise ValueError(action)
    start, count = ranges[action]
    return list(range(start, start + count))


def extract_strip(image, selected_columns):
    if image.size != (4096, 1024) or not selected_columns or any(c not in range(32) for c in selected_columns):
        raise ValueError("原图尺寸或列号错误")
    output = Image.new("RGBA", (128 * len(selected_columns), 1024))
    for index, column in enumerate(selected_columns):
        output.paste(image.crop((column * 128, 0, (column + 1) * 128, 1024)), (index * 128, 0))
    return output


def compose_hero(layers):
    if set(layers) != set(LAYER_FILES) or any(image.size != (4096, 1024) for image in layers.values()):
        raise ValueError("必须提供同尺寸的四张原图层")
    output = Image.new("RGBA", (4096, 1024))
    for row in range(8):
        bounds = (0, row * 128, 4096, (row + 1) * 128)
        rendered = Image.new("RGBA", (4096, 128))
        for layer in layer_order(row):
            rendered = Image.alpha_composite(rendered, layers[layer].convert("RGBA").crop(bounds))
        output.paste(rendered, (0, row * 128))
    return output


def preview_order(action, count):
    if action == "Idle":
        return list(range(count)) + list(range(count - 2, 0, -1))
    return list(range(count))


def reference_ms(actor, action):
    return {"Idle": 180 if actor == "Hero" else 120, "Move": 60, "Attack": 90, "Hit": 60, "Die": 120}[action]


def font(size):
    return ImageFont.truetype(str(FONT_FILE), size) if FONT_FILE.exists() else ImageFont.load_default(size=size)


def checker(size, tile=16):
    image = Image.new("RGB", size, (38, 42, 50))
    draw = ImageDraw.Draw(image)
    for y in range(0, size[1], tile):
        for x in range(0, size[0], tile):
            if (x // tile + y // tile) % 2:
                draw.rectangle((x, y, x + tile - 1, y + tile - 1), fill=(47, 52, 61))
    return image


def frame(image, column, row):
    return image.crop((column * 128, row * 128, (column + 1) * 128, (row + 1) * 128))


def draw_cell(canvas, sprite, origin, scale=2, anchor=False):
    x, y = origin
    if scale != 1:
        sprite = sprite.resize((128 * scale, 128 * scale), Image.Resampling.NEAREST)
    canvas.paste(sprite, (x, y), sprite)
    if anchor:
        draw = ImageDraw.Draw(canvas)
        ax, ay = x + 64 * scale, y + 96 * scale
        draw.line((ax - 5, ay, ax + 5, ay), fill=(100, 180, 170), width=1)
        draw.line((ax, ay - 5, ax, ay + 5), fill=(100, 180, 170), width=1)


def overview(images):
    canvas = checker((1120, 720))
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 16), "Flare 小样｜剑盾英雄 + 骷髅｜离线图片，未导入 UE", font=font(23), fill="white")
    draw.text((24, 50), "原生画布 128×128；预览 1× / 3× 最近邻；十字为参考地面锚点 (64,96)", font=font(16), fill=(185, 196, 205))
    for index, actor in enumerate(("Hero", "Skeleton")):
        x = 30 + index * 555
        draw.text((x, 95), "钢甲剑盾英雄" if actor == "Hero" else "骷髅战士", font=font(22), fill="white")
        draw_cell(canvas, frame(images[actor], 0, 5), (x, 143), 1, True)
        draw_cell(canvas, frame(images[actor], 0, 5), (x + 145, 110), 3, True)
        draw.text((x, 292), "1× 原尺寸", font=font(16), fill="white")
        draw.text((x + 170, 477), "3× 仅放大查看，资源未放大", font=font(16), fill="white")
        for direction in range(8):
            dx, dy = x + (direction % 4) * 128, 517 + (direction // 4) * 84
            sprite = frame(images[actor], 0, direction)
            sprite = sprite.crop((16, 26, 112, 113))
            canvas.paste(sprite, (dx, dy), sprite)
            draw.text((dx + 86, dy + 14), DIRECTIONS[direction], font=font(13), fill=(194, 218, 232))
    canvas.save(ROOT / "Preview/Overview.png")


def contact_sheet(images, actor):
    canvas = checker((1120, 820))
    draw = ImageDraw.Draw(canvas)
    draw.text((18, 12), f"{actor}｜SE 朝向原始帧序｜1×｜离线预览，未在 UE 验证", font=font(22), fill="white")
    names = {"Idle": "待机", "Move": "移动", "Attack": "攻击", "Hit": "受击", "Die": "死亡"}
    for action_index, action in enumerate(ACTIONS):
        y = 66 + action_index * 147
        draw.text((10, y + 20), names[action], font=font(17), fill="white")
        for index, column in enumerate(columns(actor, action)):
            x = 88 + index * 128
            draw_cell(canvas, frame(images[actor], column, 5), (x, y), 1, True)
            draw.text((x + 20, y + 124), f"{index} (源列 {column})", font=font(12), fill=(185, 196, 205))
    canvas.save(ROOT / f"Preview/{actor}_SE_FrameOrder.png")


def animate(images, action):
    count = len(columns("Hero", action))
    order = preview_order(action, count)
    durations = [reference_ms(actor, action) for actor in ("Hero", "Skeleton")]
    period = max(len(order) * duration for duration in durations)
    if durations[0] != durations[1]:
        from math import lcm
        period = lcm(*(len(order) * duration for duration in durations))
    if action in ("Attack", "Hit", "Die"):
        period += 600
    frames = []
    for elapsed in range(0, period, 30):
        canvas = checker((2112, 660))
        draw = ImageDraw.Draw(canvas)
        label = {"Idle": "待机", "Move": "移动", "Attack": "攻击", "Hit": "受击", "Die": "死亡"}[action]
        draw.text((18, 10), f"{label}｜八方向｜2× 最近邻预览｜Flare v0.14 参考时序，非原包声明 FPS", font=font(23), fill="white")
        for actor_index, actor in enumerate(("Hero", "Skeleton")):
            y = 92 + actor_index * 285
            source_columns = columns(actor, action)
            sequence_index = elapsed // reference_ms(actor, action)
            if action in ("Idle", "Move"):
                sequence_index %= len(order)
            else:
                sequence_index = min(sequence_index, len(order) - 1)
            column = source_columns[order[sequence_index]]
            ms = reference_ms(actor, action)
            draw.text((20, y - 26), f"{actor}｜{len(source_columns)} 源帧/方向｜{ms} ms/帧", font=font(17), fill="white")
            for row, direction in enumerate(DIRECTIONS):
                x = 20 + row * 260
                draw_cell(canvas, frame(images[actor], column, row), (x, y), 2, True)
                draw.text((x + 10, y + 6), direction, font=font(17), fill=(197, 216, 230))
        frames.append(canvas)
    frames[0].save(ROOT / f"Preview/{action}_8Directions.gif", save_all=True, append_images=frames[1:], duration=30, loop=0, optimize=False, disposal=2)


def main():
    for directory in ("Prepared/Hero", "Prepared/Skeleton", "Preview"):
        (ROOT / directory).mkdir(parents=True, exist_ok=True)
    sources = json.loads((ROOT / "sources.json").read_text(encoding="utf-8"))
    for record in sources["files"]:
        assert sha256((ROOT / record["file"]).read_bytes()).hexdigest() == record["sha256"]
    layers = {key: Image.open(ROOT / "Original/Hero" / name).convert("RGBA") for key, name in LAYER_FILES.items()}
    images = {"Hero": compose_hero(layers), "Skeleton": Image.open(ROOT / "Original/Skeleton/skeleton_0.png").convert("RGBA")}
    skeleton_pixels = sha256(images["Skeleton"].tobytes()).hexdigest()
    assert skeleton_pixels == "5825924d986e8b0c2aca5d597dbf9eee22e1913d43bd7405a72e3c9a53990231", skeleton_pixels
    images["Hero"].save(ROOT / "Prepared/Hero/Hero_SteelSwordShield_Full.png")
    records, issues = [], []
    total_frames = 0
    for actor, image in images.items():
        for action in ACTIONS:
            selected = columns(actor, action)
            strip = extract_strip(image, selected)
            path = ROOT / f"Prepared/{actor}/{actor}_{action}_8Directions.png"
            strip.save(path)
            reopened = Image.open(path).convert("RGBA")
            assert reopened.tobytes() == strip.tobytes()
            for row in range(8):
                for index, column in enumerate(selected):
                    actual = frame(reopened, index, row)
                    expected = frame(image, column, row)
                    assert actual.tobytes() == expected.tobytes()
                    alpha = actual.getchannel("A")
                    bounds = alpha.getbbox()
                    if bounds is None:
                        issues.append({"actor": actor, "action": action, "row": row, "frame": index, "issue": "empty_frame"})
                    elif bounds[0] == 0 or bounds[1] == 0 or bounds[2] == 128 or bounds[3] == 128:
                        issues.append({"actor": actor, "action": action, "row": row, "frame": index, "issue": "source_alpha_touches_cell_edge", "bounds": bounds})
                    total_frames += 1
            ms = reference_ms(actor, action)
            records.append({"file": path.relative_to(ROOT).as_posix(), "actor": actor, "action": action,
                "image_size": list(strip.size), "frame_size": [128, 128], "directions": list(DIRECTIONS),
                "source_columns_zero_based": selected, "source_frames_per_direction": len(selected),
                "pivot_pixel_from_top_left": [64, 96], "pivot_normalized": [0.5, 0.75],
                "source_release_fps": None, "preview_reference_ms_per_frame": ms, "preview_reference_fps": 1000 / ms,
                "timing_basis": "官方 Flare v0.14 动画定义，仅作小样参考，不是原包声明的 FPS", "source_frame_order": list(range(len(selected))),
                "preview_order": preview_order(action, len(selected)), "preview_pause_after_once_ms": 600 if action in ("Attack", "Hit", "Die") else 0,
                "preview_loop_is_demonstration_only": True, "rgba_sha256": sha256(strip.tobytes()).hexdigest(),
                "file_sha256": sha256(path.read_bytes()).hexdigest(), "ue_imported": False, "ue_verified": False})
    overview(images)
    for actor in images:
        contact_sheet(images, actor)
    for action in ACTIONS:
        animate(images, action)
        print(f"PREVIEW {action}", flush=True)
    report = {"status": "original_and_prepared_images_only", "frame_size": [128, 128], "directions": list(DIRECTIONS),
        "ue_imported": False, "ue_verified": False, "source_release_fps": None,
        "modifications": ["英雄按方向合成四层", "按动作抽取列，不缩放、不裁掉透明边", "骷髅 PNG 转换为 RGBA8，像素值与官方 v0.14 原图一致", "另制带标签的离线预览"],
        "prepared": records, "verification": {"checked_frame_cells": total_frames, "source_pixel_mismatch": 0,
            "source_hashes_checked": len(sources["files"]), "skeleton_rgba_sha256": skeleton_pixels,
            "cell_edge_or_empty_findings": issues}, "engine_integration": "PNG -> Texture2D -> 128x128 网格 PaperSprite -> 每动作每方向 PaperFlipbook；参考锚点(64,96)。插件及材质/朝向逻辑交框架负责人。"}
    (ROOT / "manifest.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    inventory = []
    for path in sorted(ROOT.rglob("*")):
        if path.is_file() and path.name != "inventory.json" and "__pycache__" not in path.parts:
            data = path.read_bytes()
            inventory.append({"file": path.relative_to(ROOT).as_posix(), "bytes": len(data), "sha256": sha256(data).hexdigest()})
    (ROOT / "inventory.json").write_text(json.dumps(inventory, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"BUILD PASS: {len(records)} action sheets; {total_frames} cells checked; {len(issues)} edge/empty findings; {len(inventory)} inventoried files", flush=True)


if __name__ == "__main__":
    main()

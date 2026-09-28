"""生成独立 UI 素材，不导入 UE、不读取或改写任何玩法文件。"""
import hashlib
import json
from pathlib import Path
import zipfile

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
PALETTE = {
    "background": "#101923", "panel": "#182633", "line": "#506779",
    "text": "#EDF3F5", "muted": "#A9BDC8", "cyan": "#55DCCF",
    "green": "#72D495", "blue": "#63AEF1", "purple": "#B995ED",
    "gold": "#E9C773", "red": "#EF7B79",
}
ASSETS = []
RASTERS = {}
FONT = Path("C:/Windows/Fonts/msyh.ttc")
FONT_BOLD = Path("C:/Windows/Fonts/msyhbd.ttc")


def rgba(color):
    if color is None:
        return None
    color = color.lstrip("#")
    values = tuple(int(color[i:i + 2], 16) for i in range(0, len(color), 2))
    return values if len(values) == 4 else values + (255,)


def svg_color(name, color):
    if color is None:
        return f'{name}="none"'
    r, g, b, a = rgba(color)
    return f'{name}="#{r:02x}{g:02x}{b:02x}" {name}-opacity="{a / 255:.6f}"'


class Vector:
    """同一组基础图形输出可编辑 SVG 和四倍采样 RGBA PNG。"""
    def __init__(self, width, height):
        self.w, self.h = width, height
        self.image = Image.new("RGBA", (width * SCALE, height * SCALE))
        self.parts = []

    def layer(self, action):
        layer = Image.new("RGBA", self.image.size)
        action(ImageDraw.Draw(layer))
        self.image = Image.alpha_composite(self.image, layer)

    def rect(self, x, y, w, h, radius=0, fill=None, stroke=None, sw=1):
        def draw(d):
            inset = sw / 2 if stroke else 0
            box = tuple(round(v * SCALE) for v in (x + inset, y + inset, x + w - inset, y + h - inset))
            d.rounded_rectangle(box, radius=max(0, radius - inset) * SCALE,
                                fill=rgba(fill), outline=rgba(stroke), width=max(1, round(sw * SCALE)))
        self.layer(draw)
        inset = sw / 2 if stroke else 0
        self.parts.append(f'<rect x="{x + inset}" y="{y + inset}" width="{w - 2 * inset}" height="{h - 2 * inset}" rx="{max(0, radius - inset)}" {svg_color("fill", fill)} {svg_color("stroke", stroke)} stroke-width="{sw}"/>')

    def ellipse(self, x, y, w, h, fill=None, stroke=None, sw=1):
        def draw(d):
            d.ellipse(tuple(round(v * SCALE) for v in (x, y, x + w, y + h)),
                      fill=rgba(fill), outline=rgba(stroke), width=max(1, round(sw * SCALE)))
        self.layer(draw)
        self.parts.append(f'<ellipse cx="{x + w / 2}" cy="{y + h / 2}" rx="{w / 2}" ry="{h / 2}" {svg_color("fill", fill)} {svg_color("stroke", stroke)} stroke-width="{sw}"/>')

    def poly(self, points, fill=None, stroke=None, sw=1, closed=True):
        def draw(d):
            pts = [(round(x * SCALE), round(y * SCALE)) for x, y in points]
            if fill:
                d.polygon(pts, fill=rgba(fill))
            if stroke:
                d.line(pts + ([pts[0]] if closed else []), fill=rgba(stroke), width=max(1, round(sw * SCALE)), joint="curve")
        self.layer(draw)
        coords = " ".join(f"{x},{y}" for x, y in points)
        tag = "polygon" if closed else "polyline"
        self.parts.append(f'<{tag} points="{coords}" {svg_color("fill", fill)} {svg_color("stroke", stroke)} stroke-width="{sw}" stroke-linejoin="round"/>')

    def export(self, asset_id, label, group, margins=(0, 0, 0, 0), tint=False, usage=""):
        image = self.image.resize((self.w, self.h), Image.Resampling.LANCZOS)
        png = ROOT / "png" / f"{asset_id}.png"
        svg = ROOT / "svg" / f"{asset_id}.svg"
        image.save(png)
        svg.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}">\n' + "\n".join(self.parts) + "\n</svg>\n", encoding="utf-8")
        RASTERS[asset_id] = image
        ASSETS.append({
            "id": asset_id, "label_zh": label, "group": group,
            "png": png.relative_to(ROOT).as_posix(), "svg": svg.relative_to(ROOT).as_posix(),
            "size_px": [self.w, self.h], "nine_slice_px": list(margins),
            "ue_margin_ltrb": [margins[0] / self.w, margins[1] / self.h, margins[2] / self.w, margins[3] / self.h],
            "draw_as": "Box" if any(margins) else "Image",
            "tint_policy": "由 Widget 着色" if tint else "保持白色 tint",
            "usage_zh": usage or label, "source_id": "original-vector-v001",
            "sha256_png": hashlib.sha256(png.read_bytes()).hexdigest(),
            "sha256_svg": hashlib.sha256(svg.read_bytes()).hexdigest(),
        })


def make_assets():
    for asset_id, label, h in (("panel_body", "分区面板", 128), ("panel_header", "折叠标题条", 64)):
        v = Vector(256, h)
        v.rect(3, 3, 250, h - 6, 12, "#182633F2", "#526779", 1.5)
        v.poly([(16, 7), (240, 7)], stroke="#B4CAD326", closed=False)
        v.export(asset_id, label, "common", (16, 16, 16, 16), usage="独立九宫格；标题条与内容分离，收起时只保留标题条。文字和三角由 Widget 叠加。")
    states = {
        "normal": ("常态", "#213342", "#5A7182"),
        "hover": ("悬停／聚焦", "#254550", "#55DCCF"),
        "pressed": ("按下", "#132A32", "#44B6AB"),
        "disabled": ("不可用", "#202B35", "#40515E"),
    }
    for state, (label, fill, border) in states.items():
        v = Vector(192, 64)
        v.rect(3, 3, 186, 58, 10, fill, border, 2)
        v.poly([(14, 8 if state != "pressed" else 55), (178, 8 if state != "pressed" else 55)], stroke=border + "66", closed=False)
        v.export(f"button_{state}", f"按钮 · {label}", "button", (14, 14, 14, 14), usage="无烘焙文字；不可用原因应另用文字说明，不能只靠颜色。")
    for direction, pts, label in (
        ("down", [(15, 23), (49, 23), (32, 43)], "展开态：点击收起"),
        ("right", [(23, 15), (43, 32), (23, 49)], "收起态：点击展开"),
    ):
        v = Vector(64, 64)
        v.poly(pts, fill="#FFFFFF")
        v.export(f"chevron_{direction}", label, "common", tint=True, usage="白色透明蒙版；视觉图标可缩至16—20像素，建议按钮点击区独立放大。")
    for color, label in (("green", "绿"), ("blue", "蓝"), ("purple", "紫"), ("gold", "金"), ("red", "红")):
        c = PALETTE[color]
        v = Vector(128, 128)
        v.rect(4, 4, 120, 120, 12, stroke=c, sw=2)
        v.rect(9, 9, 110, 110, 8, stroke=c + "48", sw=1)
        v.poly([(16, 4), (36, 4)], stroke=c, sw=4, closed=False)
        v.export(f"quality_{color}", f"品质框 · {label}", "slot", (18, 18, 18, 18), usage="中间透明；品质文字不烘焙进贴图，由 Widget 补充，不能只靠边框颜色。")
    v = Vector(128, 128)
    for a, b in ((16, 34), (46, 64), (76, 94), (104, 112)):
        for pts in ([(a, 5), (b, 5)], [(a, 123), (b, 123)], [(5, a), (5, b)], [(123, a), (123, b)]):
            v.poly(pts, stroke="#728693", sw=2, closed=False)
    v.poly([(52, 64), (76, 64)], stroke="#728693", sw=2, closed=False)
    v.export("slot_empty", "空槽", "slot", usage="含中心空槽短线，使用等比 Image，不做九宫格。")
    for mode, label, c in (("active", "主动技能框", PALETTE["cyan"]), ("passive", "被动技能框", "#819CAC")):
        v = Vector(128, 128)
        v.rect(4, 4, 120, 120, 12, stroke=c, sw=2.5)
        if mode == "active":
            v.rect(8, 8, 112, 112, 9, stroke=c + "55", sw=1.5)
        else:
            v.poly([(14, 114), (28, 114)], stroke=c, sw=3, closed=False)
        v.export(f"skill_{mode}_frame", label, "slot", (18, 18, 18, 18), usage="类型文字另叠加；被动不是禁用状态，不使用锁或施放提示。")
    v = Vector(128, 128)
    v.rect(9, 9, 110, 110, 8, fill="#07111AB0")
    v.export("cooldown_mask", "冷却暗层", "slot", usage="仅整槽暗层，不包含进度、倒计时或动画；数字及动态裁切以后由界面层处理。")
    for name, label, w, h, fill, border in (
        ("health_track", "单位血条底轨", 256, 32, "#0D1820E8", "#526B7B"),
        ("health_frame", "单位血条外框", 256, 32, None, "#86A4B4"),
        ("boss_track", "BOSS 血条底轨", 512, 48, "#201A22ED", "#725562"),
        ("boss_frame", "BOSS 血条外框", 512, 48, None, "#DB9992"),
    ):
        v = Vector(w, h)
        v.rect(3, 3, w - 6, h - 6, 10, fill, border, 1.5)
        v.export(name, label, "health", (14, 14, 14, 14), usage="底轨、填充、外框分层；无生命数字、护盾或延迟扣血语义。参考填充内缩左右10px、上下8px。")
    v = Vector(256, 16)
    v.rect(0, 0, 256, 16, fill="#FFFFFF")
    v.export("health_fill_white", "通用可着色填充", "health", tint=True, usage="纯白无纹理RGBA，矩形内部全不透明是有意设计；UI按类型着色并按比例裁切，不拉伸外框来表示生命。")
    icon_labels = {
        "coin": "金币", "experience": "经验", "damage": "伤害",
        "training": "练功房", "home": "回城", "tower": "爬塔",
        "shop": "商店", "technology": "科技", "active": "主动占位", "passive": "被动占位",
    }
    for name, label in icon_labels.items():
        v = Vector(64, 64)
        white = "#FFFFFF"
        if name == "coin":
            v.ellipse(12, 12, 40, 40, stroke=white, sw=3)
            v.ellipse(18, 18, 28, 28, stroke=white, sw=1.5)
            v.poly([(32, 24), (40, 32), (32, 40), (24, 32)], fill=white)
        elif name == "experience":
            v.poly([(32, 9), (38, 25), (55, 27), (42, 38), (46, 54), (32, 45), (18, 54), (22, 38), (9, 27), (26, 25)], stroke=white, sw=3)
        elif name in ("damage", "active"):
            v.poly([(35, 9), (16, 36), (29, 36), (25, 55), (50, 26), (36, 26), (42, 9)], fill=white)
        elif name == "training":
            v.ellipse(12, 12, 40, 40, stroke=white, sw=3)
            v.ellipse(23, 23, 18, 18, stroke=white, sw=3)
            v.poly([(32, 4), (32, 16)], stroke=white, sw=3, closed=False)
            v.poly([(48, 32), (60, 32)], stroke=white, sw=3, closed=False)
        elif name == "home":
            v.poly([(10, 30), (32, 11), (54, 30)], stroke=white, sw=3, closed=False)
            v.poly([(17, 28), (17, 53), (47, 53), (47, 28)], stroke=white, sw=3, closed=False)
            v.poly([(27, 52), (27, 37), (37, 37), (37, 52)], stroke=white, sw=3, closed=False)
        elif name == "tower":
            v.poly([(16, 53), (20, 28), (17, 28), (17, 12), (25, 12), (25, 20), (30, 20), (30, 12), (36, 12), (36, 20), (41, 20), (41, 12), (48, 12), (48, 28), (44, 28), (48, 53)], stroke=white, sw=3)
            v.rect(28, 38, 9, 15, 4, stroke=white, sw=2)
        elif name == "shop":
            v.poly([(10, 28), (16, 13), (48, 13), (54, 28)], stroke=white, sw=3)
            v.poly([(15, 28), (15, 52), (49, 52), (49, 28)], stroke=white, sw=3, closed=False)
            v.poly([(23, 14), (21, 28), (32, 28), (32, 14), (32, 28), (43, 28), (41, 14)], stroke=white, sw=2, closed=False)
            v.rect(23, 37, 12, 15, stroke=white, sw=2)
        elif name == "technology":
            v.poly([(32, 17), (32, 30), (14, 30), (14, 44)], stroke=white, sw=3, closed=False)
            v.poly([(32, 30), (50, 30), (50, 44)], stroke=white, sw=3, closed=False)
            for x, y in ((25, 7), (7, 44), (43, 44)):
                v.rect(x, y, 14, 12, 3, stroke=white, sw=3)
        else:
            v.ellipse(12, 12, 40, 40, stroke=white, sw=3)
            v.ellipse(23, 23, 18, 18, stroke=white, sw=2)
            v.ellipse(29, 29, 6, 6, fill=white)
        v.export(f"icon_{name}", f"图标 · {label}", "icon", tint=True, usage="原创基础几何符号；64px母版，建议32px以上展示，配独立文字标签，不等于正式技能/装备条目。")
    v = Vector(256, 256)
    v.ellipse(16, 16, 224, 224, fill="#17293722", stroke="#9DB8C055", sw=2)
    v.ellipse(32, 32, 192, 192, stroke="#9DB8C030", sw=1.5)
    for pts in ([(128, 17), (128, 31)], [(128, 225), (128, 239)], [(17, 128), (31, 128)], [(225, 128), (239, 128)]):
        v.poly(pts, stroke="#C3D9E070", sw=2, closed=False)
    v.export("joystick_base", "移动摇杆底环", "input", usage="触控方案尚未定案，仅预备视觉素材，不含输入逻辑。")
    v = Vector(96, 96)
    v.ellipse(8, 8, 80, 80, fill="#2B455AC0", stroke="#91B4C4B0", sw=2)
    v.ellipse(15, 15, 66, 66, stroke="#AACAD540", sw=1)
    v.export("joystick_knob", "移动摇杆手柄", "input", usage="与底环独立使用，不能凭素材交付宣称触控输入完成。")


def nine_slice(image, size, margins):
    w, h = image.size
    tw, th = size
    l, t, r, b = margins
    if not any(margins):
        return image.resize(size, Image.Resampling.LANCZOS)
    if tw < l + r or th < t + b:
        raise ValueError("目标尺寸不能小于九宫格固定边缘")
    xs, ys = [0, l, w - r, w], [0, t, h - b, h]
    dx, dy = [0, l, tw - r, tw], [0, t, th - b, th]
    result = Image.new("RGBA", size)
    for row in range(3):
        for col in range(3):
            tile = image.crop((xs[col], ys[row], xs[col + 1], ys[row + 1]))
            target = (dx[col + 1] - dx[col], dy[row + 1] - dy[row])
            if target[0] and target[1]:
                result.alpha_composite(tile.resize(target, Image.Resampling.LANCZOS), (dx[col], dy[row]))
    return result


def put(image, name, xy, size=None, tint=None):
    source = RASTERS[name]
    if size:
        asset = next(a for a in ASSETS if a["id"] == name)
        source = nine_slice(source, size, asset["nine_slice_px"])
    if tint:
        colored = Image.new("RGBA", source.size, rgba(tint))
        colored.putalpha(source.getchannel("A"))
        source = colored
    image.alpha_composite(source, xy)


def text(image, xy, value, size=22, color=None, bold=False):
    ImageDraw.Draw(image).text(xy, value, fill=color or PALETTE["text"], font=ImageFont.truetype(str(FONT_BOLD if bold else FONT), size))


def panel(image, xy, size, title, collapsed=False):
    x, y = xy
    w, h = size
    if not collapsed:
        put(image, "panel_body", (x, y), (w, h))
    put(image, "panel_header", (x, y), (w, 48))
    text(image, (x + 16, y + 9), title, 22, bold=True)
    put(image, "chevron_right" if collapsed else "chevron_down", (x + w - 44, y + 8), (32, 32), PALETTE["cyan"])


def health(image, x, y, width, fraction, color, boss=False):
    track = "boss_track" if boss else "health_track"
    frame = "boss_frame" if boss else "health_frame"
    height = 40 if boss else 28
    put(image, track, (x, y), (width, height))
    fill_width = round((width - 20) * fraction)
    if fill_width > 0:
        put(image, "health_fill_white", (x + 10, y + 8), (fill_width, height - 16), color)
    put(image, frame, (x, y), (width, height))


def contact_sheet():
    image = Image.new("RGBA", (1800, 2140), PALETTE["background"])
    text(image, (40, 24), "Y01 / UI 素材包  v001", 42, bold=True)
    text(image, (40, 84), "34 个独立素材 · 每个均提供透明 RGBA PNG + 可编辑 SVG", 24)
    text(image, (40, 124), "文字不烘焙进素材  /  白色图标与填充由 Widget 着色  /  无业务逻辑", 20, PALETTE["muted"])
    d = ImageDraw.Draw(image)
    for i, asset in enumerate(ASSETS):
        x, y = 32 + i % 4 * 440, 184 + i // 4 * 208
        d.rounded_rectangle((x, y, x + 424, y + 196), 10, fill="#1B2936", outline="#334858")
        text(image, (x + 14, y + 10), f'{i + 1:02}  {asset["label_zh"]}', 19)
        for cx in range(x + 12, x + 412, 16):
            for cy in range(y + 42, y + 138, 16):
                d.rectangle((cx, cy, cx + 15, cy + 15), fill="#263643" if (cx + cy) // 16 % 2 else "#30414E")
        source = RASTERS[asset["id"]]
        scale = min(360 / source.width, 86 / source.height, 1.25)
        thumb = source.resize((round(source.width * scale), round(source.height * scale)), Image.Resampling.LANCZOS)
        image.alpha_composite(thumb, (x + (424 - thumb.width) // 2, y + 46 + (86 - thumb.height) // 2))
        text(image, (x + 14, y + 143), asset["id"], 16, PALETTE["muted"])
        text(image, (x + 14, y + 167), f'{asset["size_px"][0]}×{asset["size_px"][1]}  ·  {"九宫格" if asset["draw_as"] == "Box" else "Image"}', 15, PALETTE["muted"])
    text(image, (40, 2098), "棋盘格仅为预览背景，不在 PNG 内。来源、尺寸、九宫格与使用说明见 manifest.json / sources.json。", 18, PALETTE["muted"])
    image.convert("RGB").save(ROOT / "previews" / "contact_sheet.png")


def composite():
    image = Image.new("RGBA", (1800, 1160), PALETTE["background"])
    d = ImageDraw.Draw(image)
    d.rectangle((20, 112, 1780, 1074), fill="#14212C")
    for x in range(20, 1781, 64):
        d.line((x, 112, x, 1074), fill="#1C2B36")
    for y in range(112, 1075, 64):
        d.line((20, y, 1780, y), fill="#1C2B36")
    text(image, (36, 22), "Y01 / HUD + World UI 素材组合", 36, bold=True)
    text(image, (1190, 36), "素材实图拼装 · 模拟数据 · 未接入 UE", 22, PALETTE["gold"])
    text(image, (36, 76), "A01 / A02 延续 · 本图由本包 PNG 实际拼装，不是生成式效果图", 20, PALETTE["muted"])
    regions = []
    def region(name, x, y, w, h, collapsed=False):
        panel(image, (x, y), (w, h), name, collapsed)
        regions.append({"name": name, "rect": [x, y, w, 48 if collapsed else h]})
    region("个人状态", 40, 132, 320, 176)
    text(image, (58, 191), "你  820 / 1000", 24)
    health(image, 56, 231, 280, .82, PALETTE["green"])
    put(image, "icon_coin", (58, 267), (28, 28), PALETTE["gold"])
    text(image, (99, 264), "局内金币 1,280", 22, PALETTE["gold"])
    region("波次阶段", 556, 132, 672, 126)
    text(image, (574, 190), "第 05 / 15 波 · 战斗中", 24)
    text(image, (953, 192), "核心摘要 75%", 22, PALETTE["muted"])
    region("BOSS · 示例首领", 556, 270, 672, 128)
    health(image, 574, 326, 634, .6, PALETTE["red"], True)
    text(image, (776, 369), "6,000 / 10,000", 16)
    region("区域与发育", 1420, 132, 340, 240)
    for i, (name, icon) in enumerate((("练功房", "training"), ("回城", "home"), ("商店", "shop"), ("发育", "technology"))):
        x, y = 1436 + i % 2 * 158, 192 + i // 2 * 64
        put(image, "button_normal", (x, y), (148, 56))
        put(image, f"icon_{icon}", (x + 10, y + 12), (30, 30), PALETTE["muted"])
        text(image, (x + 48, y + 12), name, 20)
    text(image, (1438, 333), "发育内：爬塔 / 科技", 18, PALETTE["muted"])
    region("伤害占比", 40, 332, 300, 240)
    text(image, (58, 390), "模拟 · 统计口径待确认", 18, PALETTE["muted"])
    for i, (name, value) in enumerate((("A（你）", 36), ("B", 29), ("C", 21), ("D", 14))):
        text(image, (58, 425 + i * 32), f"玩家 {name}", 20, PALETTE["cyan"] if i == 0 else None)
        text(image, (252, 425 + i * 32), f"{value}%", 20)
    region("移动（假设）", 48, 782, 270, 48, True)
    region("装备 · 6槽", 410, 858, 772, 190)
    for i, quality in enumerate(("green", "blue", "purple", "gold", "red", None)):
        x, y = 426 + i * 123, 914
        put(image, f"quality_{quality}" if quality else "slot_empty", (x, y), (110, 110))
        if quality:
            put(image, "icon_passive", (x + 28, y + 18), (54, 54), PALETTE[quality])
        text(image, (x + 24, y + 76), f'{i + 1} {("绿", "蓝", "紫", "金", "红", "空")[i]}', 18)
    region("技能 · 4槽", 1378, 639, 382, 409)
    for i in range(4):
        x, y = 1394 + i % 2 * 176, 697 + i // 2 * 169
        active = i < 2
        put(image, "skill_active_frame" if active else "skill_passive_frame", (x, y), (162, 156))
        put(image, "icon_active" if active else "icon_passive", (x + 46, y + 36), (68, 68), PALETTE["cyan"] if active else PALETTE["muted"])
        if i == 1:
            put(image, "cooldown_mask", (x, y), (162, 156))
        text(image, (x + 14, y + 10), f'{i + 1} {"主动" if active else "被动"}', 20)
        text(image, (x + 18, y + 119), ("可用", "冷却 6.2s", "生效中", "生效中")[i], 19, PALETTE["text"])
    # 中性几何仅用于挂点示意，不是非 UI 美术交付。
    units = [(520, 486, "战利品仓", .75, "cyan"), (1055, 486, "小怪", .5, "red"),
             (520, 691, "玩家 A · 自己", .82, "green"), (1055, 691, "玩家 B · 队友", .75, "blue")]
    for x, y, name, fraction, color in units:
        text(image, (x, y), name, 23)
        health(image, x, y + 36, 244, fraction, PALETTE[color])
        d.ellipse((x + 86, y + 84, x + 140, y + 116), fill="#273E4D", outline=PALETTE[color], width=2)
        d.line((x + 113, y + 64, x + 113, y + 84), fill="#6C8596", width=1)
    put(image, "icon_damage", (1180, 591), (26, 26), PALETTE["red"])
    text(image, (1208, 584), "−20 伤害", 23, PALETTE["red"])
    text(image, (760, 725), "+12 金币", 25, PALETTE["gold"])
    text(image, (760, 765), "+30 经验", 25, PALETTE["blue"])
    text(image, (40, 1090), "所有文本、数值和比例仅作组合示例；素材不包含字体、数值、结算、绑定、动画或输入逻辑。", 20, PALETTE["muted"])
    text(image, (40, 1123), "折叠示例：移动只保留标题条。World 血条由底轨＋着色填充＋外框组成，无新增护盾机制。", 20, PALETTE["muted"])
    image.convert("RGB").save(ROOT / "previews" / "hud_world_composite.png")
    write_json("preview_layout.json", {"simulated": True, "equipment_slot_count": 6,
        "skill_types": ["active", "active", "passive", "passive"],
        "world_units": ["warehouse", "self", "teammate", "monster"],
        "floating_types": ["damage", "gold", "experience"], "regions": regions,
        "note_zh": "布局元数据仅记录静态预览，不是可运行 Widget，也未进行 UE 或触控验收。"})


def slice_preview():
    image = Image.new("RGBA", (1500, 660), PALETTE["background"])
    text(image, (32, 24), "九宫格与分层验证 / 固定边角，拉伸中段", 32, bold=True)
    for i, size in enumerate(((192, 64), (320, 64), (600, 64))):
        x = (40, 310, 720)[i]
        put(image, "button_hover", (x, 124), size)
        text(image, (x, 200), f"{size[0]} × {size[1]}", 20)
    text(image, (40, 272), "生命填充独立裁切：25% / 50% / 75% / 100%（示例值）", 22)
    for i, fraction in enumerate((.25, .5, .75, 1)):
        health(image, 40 + i * 360, 324, 320, fraction, PALETTE["green"])
        text(image, (40 + i * 360, 366), f"{round(fraction * 100)}%", 20)
    panel(image, (40, 452), (580, 150), "展开分区", False)
    text(image, (60, 518), "内容与标题分开使用", 22)
    panel(image, (790, 452), (580, 48), "收起分区", True)
    text(image, (790, 528), "只保留标题，不留大空框", 22, PALETTE["muted"])
    image.convert("RGB").save(ROOT / "previews" / "nine_slice_check.png")


def write_json(name, data):
    (ROOT / name).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def metadata():
    write_json("manifest.json", {"pack": "Y01_UI_Assets_v001", "collaboration": "COLLAB-03", "requirements": ["A01", "A02", "R27"],
        "asset_count": len(ASSETS), "ue_import_verified": False, "business_logic_connected": False,
        "palette": PALETTE,
        "scope_zh": "仅独立素材：不含 Widget、角色挂接、GAS、RPC、结算或输入实现；不含字体，不改变任何工程配置。",
        "suggested_ue_import_zh": ["PNG 用作未来的 Texture2D；SVG 为编辑源，不假定 UE 直接支持导入。",
            "导入时建议 Compression=UserInterface2D (RGBA)、TextureGroup=UI、sRGB=true、MipGenSettings=NoMipmaps、AddressX/Y=Clamp；均未在 UE 实测。",
            "Box 九宫格的 Margin 按每项 ue_margin_ltrb 顺序 Left/Top/Right/Bottom；Image 项保持等比或按用途裁切。",
            "白色填充和图标通过 Widget Tint 着色；已着色框/底板保持白色 Tint，避免二次变色。",
            "字体、数字、标签、进度比例、透明度和动画都由以后获授权的 Widget 设置；不烘焙在贴图里。",
            "素材就绪不表示 UE 已导入或真实逻辑接入；先确认目录归属与编辑器时段。"], "assets": ASSETS})
    write_json("sources.json", {"source_id": "original-vector-v001", "created": "2026-09-24", "source_zh": "本次使用原创几何指令和 Python/Pillow 绘制；SVG 与 PNG 同源生成。没有从此前概念图裁切或再分发其中图标。",
        "authoring_source": "tools/build_asset_pack.py", "third_party_assets": [], "purchased_resources": [],
        "license_status_zh": "本包为本任务中新创作的基础几何视觉素材，未引入第三方素材许可证。权利归属及商用使用遵循用户与所用服务的适用条款，本包不额外宣称 CC0、公共领域或排他权；正式对外发行前由项目确认适用条款。",
        "attribution_zh": "没有第三方素材作者署名义务记录；保留本来源清单用于项目追溯。未核验的服务条款义务不据此免除。",
        "fonts_zh": "仅预览 PNG 使用本机 Microsoft YaHei 字体渲染，未复制或打包字体文件；独立素材 PNG/SVG 内无文字。正式 UI 字体需另行确认许可。",
        "tooling_zh": "生成工具依赖 Python 3 与 Pillow（环境已安装）。本包不分发 Python、Pillow 或其字体文件。",
        "unverified_zh": ["UE 导入和材质显示", "移动端缩放及触控尺寸", "任何真实数据、网络或动画表现"]})


def archive():
    paths = []
    for folder in ("png", "svg", "previews", "tools"):
        paths.extend(p for p in (ROOT / folder).iterdir() if p.is_file())
    paths.extend(ROOT / name for name in ("manifest.json", "sources.json", "preview_layout.json"))
    with zipfile.ZipFile(ROOT / "Y01_UI_Assets_v001.zip", "w", zipfile.ZIP_DEFLATED) as zipped:
        for path in sorted(paths):
            zipped.write(path, path.relative_to(ROOT).as_posix())


def main():
    if ROOT.name != "HUD_A02_R27_Assets_v001" or ROOT.parent.name != "UI_Deliverables":
        raise RuntimeError("生成范围必须是独立素材交付目录")
    if not FONT.is_file() or not FONT_BOLD.is_file():
        raise RuntimeError("缺少预览字体；不自动下载字体")
    for directory in ("png", "svg", "previews"):
        (ROOT / directory).mkdir(exist_ok=True)
    make_assets()
    metadata()
    contact_sheet()
    composite()
    slice_preview()
    archive()
    print(f"GENERATED assets={len(ASSETS)} png={len(ASSETS)} svg={len(ASSETS)} previews=3")
    print(f"OUTPUT {ROOT}")
    print("UE_IMPORT=False BUSINESS_LOGIC=False")


if __name__ == "__main__":
    main()

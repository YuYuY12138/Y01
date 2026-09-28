"""独立素材包验收：不访问工程 Content、Source 或配置。"""
import hashlib
import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET
import zipfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    "panel_body", "panel_header",
    "button_normal", "button_hover", "button_pressed", "button_disabled",
    "chevron_down", "chevron_right",
    "quality_green", "quality_blue", "quality_purple", "quality_gold", "quality_red",
    "slot_empty", "skill_active_frame", "skill_passive_frame", "cooldown_mask",
    "health_track", "health_frame", "health_fill_white", "boss_track", "boss_frame",
    "icon_coin", "icon_experience", "icon_damage", "icon_training", "icon_home",
    "icon_tower", "icon_shop", "icon_technology", "icon_active", "icon_passive",
    "joystick_base", "joystick_knob",
}


class AssetPackTests(unittest.TestCase):
    def manifest(self):
        path = ROOT / "manifest.json"
        self.assertTrue(path.is_file(), "素材清单尚未生成")
        return json.loads(path.read_text(encoding="utf-8"))

    def test_exact_asset_inventory(self):
        manifest = self.manifest()
        ids = [asset["id"] for asset in manifest["assets"]]
        self.assertEqual(len(ids), 34)
        self.assertEqual(set(ids), EXPECTED)
        self.assertEqual(len(list((ROOT / "png").glob("*.png"))), 34)
        self.assertEqual(len(list((ROOT / "svg").glob("*.svg"))), 34)

    def test_rgba_sizes_and_transparent_backgrounds(self):
        for asset in self.manifest()["assets"]:
            with self.subTest(asset=asset["id"]):
                with Image.open(ROOT / asset["png"]) as image:
                    self.assertEqual(image.mode, "RGBA")
                    self.assertEqual(image.size, tuple(asset["size_px"]))
                    alpha = image.getchannel("A")
                    self.assertGreater(alpha.getextrema()[1], 0)
                    if asset["id"] != "health_fill_white":
                        self.assertEqual(alpha.getextrema()[0], 0)
                        self.assertEqual(image.getpixel((0, 0))[3], 0)
                    else:
                        self.assertEqual(image.getpixel((128, 8)), (255, 255, 255, 255))

    def test_svg_is_editable_vector_without_embedded_fonts_or_images(self):
        forbidden = {"image", "text", "script", "foreignObject", "use"}
        for asset in self.manifest()["assets"]:
            with self.subTest(asset=asset["id"]):
                path = ROOT / asset["svg"]
                text = path.read_text(encoding="utf-8")
                tree = ET.fromstring(text)
                self.assertEqual(tree.attrib["viewBox"], f'0 0 {asset["size_px"][0]} {asset["size_px"][1]}')
                self.assertTrue(forbidden.isdisjoint({e.tag.split("}")[-1] for e in tree.iter()}))
                self.assertNotIn("base64", text)
                self.assertNotIn("href=", text)

    def test_nine_slice_margins_and_tint_policy(self):
        for asset in self.manifest()["assets"]:
            with self.subTest(asset=asset["id"]):
                px = asset["nine_slice_px"]
                normalized = asset["ue_margin_ltrb"]
                self.assertEqual(len(px), 4)
                self.assertEqual(len(normalized), 4)
                w, h = asset["size_px"]
                self.assertLess(px[0] + px[2], w)
                self.assertLess(px[1] + px[3], h)
                for actual, expected in zip(normalized, [px[0] / w, px[1] / h, px[2] / w, px[3] / h]):
                    self.assertAlmostEqual(actual, expected, places=6)
                self.assertIn(asset["draw_as"], {"Image", "Box"})
                self.assertTrue(asset["usage_zh"])
                self.assertIn(asset["tint_policy"], {"保持白色 tint", "由 Widget 着色"})

    def test_asset_hashes_and_licensing_records(self):
        manifest = self.manifest()
        for asset in manifest["assets"]:
            for kind in ("png", "svg"):
                actual = hashlib.sha256((ROOT / asset[kind]).read_bytes()).hexdigest()
                self.assertEqual(actual, asset[f"sha256_{kind}"])
            self.assertEqual(asset["source_id"], "original-vector-v001")
        sources = json.loads((ROOT / "sources.json").read_text(encoding="utf-8"))
        self.assertEqual(sources["third_party_assets"], [])
        self.assertEqual(sources["purchased_resources"], [])
        self.assertTrue(sources["license_status_zh"])
        self.assertTrue(sources["attribution_zh"])
        self.assertFalse(manifest["ue_import_verified"])
        self.assertFalse(manifest["business_logic_connected"])

    def test_button_states_are_visually_distinct_and_frames_hollow(self):
        manifest = self.manifest()
        assets = {a["id"]: a for a in manifest["assets"]}
        hashes = {assets[f"button_{state}"]["sha256_png"] for state in ("normal", "hover", "pressed", "disabled")}
        self.assertEqual(len(hashes), 4)
        for name in ("health_frame", "boss_frame", "skill_active_frame", "skill_passive_frame", "quality_red"):
            with Image.open(ROOT / assets[name]["png"]) as image:
                self.assertEqual(image.getpixel((image.width // 2, image.height // 2))[3], 0)

    def test_previews_and_composition_coverage(self):
        self.manifest()
        with Image.open(ROOT / "previews" / "contact_sheet.png") as image:
            self.assertEqual(image.size, (1800, 2140))
        with Image.open(ROOT / "previews" / "hud_world_composite.png") as image:
            self.assertEqual(image.size, (1800, 1160))
        with Image.open(ROOT / "previews" / "nine_slice_check.png") as image:
            self.assertEqual(image.size, (1500, 660))
        sample = json.loads((ROOT / "preview_layout.json").read_text(encoding="utf-8"))
        self.assertEqual(sample["equipment_slot_count"], 6)
        self.assertEqual(sample["skill_types"], ["active", "active", "passive", "passive"])
        self.assertEqual(sample["world_units"], ["warehouse", "self", "teammate", "monster"])
        self.assertEqual(sample["floating_types"], ["damage", "gold", "experience"])
        self.assertTrue(sample["simulated"])
        for region in sample["regions"]:
            x, y, w, h = region["rect"]
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertLessEqual(x + w, 1800)
            self.assertLessEqual(y + h, 1160)

    def test_zip_has_exact_inventory_and_no_game_assets(self):
        self.manifest()
        archive = ROOT / "Y01_UI_Assets_v001.zip"
        self.assertTrue(archive.is_file())
        with zipfile.ZipFile(archive) as zipped:
            self.assertIsNone(zipped.testzip())
            names = zipped.namelist()
            self.assertEqual(len([n for n in names if n.startswith("png/")]), 34)
            self.assertEqual(len([n for n in names if n.startswith("svg/")]), 34)
            self.assertTrue({"manifest.json", "sources.json", "previews/contact_sheet.png", "previews/hud_world_composite.png"}.issubset(names))
            self.assertFalse(any(n.startswith(("Content/", "Source/", "Config/")) for n in names))
            self.assertFalse(any(n.endswith((".uasset", ".umap", ".ttf", ".ttc", ".otf")) for n in names))
            for name in names:
                self.assertEqual(zipped.read(name), (ROOT / name).read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)

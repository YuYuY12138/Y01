"""只校验离线美术处理，不涉及 UE 或游戏代码。"""
import importlib.util
from pathlib import Path
import sys
import unittest

from PIL import Image

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).with_name("build_sample.py")


class SampleTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "尚未实现美术小样处理器")
        spec = importlib.util.spec_from_file_location("sample", SCRIPT)
        self.sample = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.sample)

    def test_directional_layer_order(self):
        for row in (0, 1, 6, 7):
            self.assertEqual(self.sample.layer_order(row), ("body", "sword", "head", "shield"))
        for row in (2, 3, 4, 5):
            self.assertEqual(self.sample.layer_order(row), ("body", "shield", "head", "sword"))
        with self.assertRaises(ValueError):
            self.sample.layer_order(8)

    def test_hit_and_death_share_columns_but_actors_differ(self):
        self.assertEqual(self.sample.columns("Hero", "Hit"), [18, 19])
        self.assertEqual(self.sample.columns("Hero", "Die"), list(range(18, 24)))
        self.assertEqual(self.sample.columns("Skeleton", "Hit"), [22, 23])
        self.assertEqual(self.sample.columns("Skeleton", "Die"), list(range(22, 28)))
        self.assertEqual(self.sample.columns("Hero", "Move"), list(range(4, 12)))
        self.assertEqual(self.sample.columns("Skeleton", "Attack"), list(range(12, 16)))
        with self.assertRaises(ValueError):
            self.sample.columns("Hero", "Invented")

    def test_strip_preserves_source_pixels_and_direction_rows(self):
        image = Image.new("RGBA", (4096, 1024))
        for row in range(8):
            for column in range(32):
                image.paste((column, row, 100, 128), (column * 128, row * 128, (column + 1) * 128, (row + 1) * 128))
        strip = self.sample.extract_strip(image, [18, 19])
        self.assertEqual(strip.size, (256, 1024))
        for row in range(8):
            self.assertEqual(strip.getpixel((0, row * 128)), (18, row, 100, 128))
            self.assertEqual(strip.getpixel((128, row * 128)), (19, row, 100, 128))
        with self.assertRaises(ValueError):
            self.sample.extract_strip(Image.new("RGBA", (128, 128)), [0])

    def test_composition_uses_nearest_hand_last(self):
        colors = {"body": (20, 0, 0, 255), "head": (40, 0, 0, 255), "sword": (60, 0, 0, 255), "shield": (80, 0, 0, 255)}
        layers = {key: Image.new("RGBA", (4096, 1024), color) for key, color in colors.items()}
        hero = self.sample.compose_hero(layers)
        for row in range(8):
            expected = colors["shield"] if row in (0, 1, 6, 7) else colors["sword"]
            self.assertEqual(hero.getpixel((64, row * 128 + 64)), expected)

    def test_preview_sequence_does_not_create_new_source_frames(self):
        self.assertEqual(self.sample.preview_order("Idle", 4), [0, 1, 2, 3, 2, 1])
        self.assertEqual(self.sample.preview_order("Die", 6), [0, 1, 2, 3, 4, 5])
        self.assertEqual(self.sample.preview_order("Move", 8), list(range(8)))

    def test_timing_is_actor_specific_reference_not_original_fps(self):
        self.assertEqual(self.sample.reference_ms("Hero", "Idle"), 180)
        self.assertEqual(self.sample.reference_ms("Skeleton", "Idle"), 120)
        self.assertEqual(self.sample.reference_ms("Hero", "Move"), 60)
        self.assertEqual(self.sample.reference_ms("Skeleton", "Attack"), 90)
        self.assertEqual(self.sample.reference_ms("Hero", "Die"), 120)


if __name__ == "__main__":
    unittest.main(verbosity=2)

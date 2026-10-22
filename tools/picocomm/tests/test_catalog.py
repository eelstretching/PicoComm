"""The Python effect and prop tables must match include/picocomm/effects.h."""

import re
import unittest
from pathlib import Path

from picocomm import catalog

HEADER = Path(__file__).resolve().parents[3] / "include" / "picocomm" / "effects.h"


def enum_values(text: str, name: str) -> dict[str, int]:
    body = re.search(r"enum class " + name + r"\b[^{]*\{(.*?)\};", text, re.S).group(1)
    return {k.lower(): int(v, 0) for k, v in re.findall(r"(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)", body)}


class CatalogTest(unittest.TestCase):
    def setUp(self):
        self.text = HEADER.read_text()

    def test_effects_match_header(self):
        self.assertEqual(catalog.EFFECTS, enum_values(self.text, "EffectId"))

    def test_prop_types_match_header(self):
        self.assertEqual(catalog.PROP_TYPES, enum_values(self.text, "PropType"))

    def test_catalog_version_matches_header(self):
        v = int(re.search(r"kCatalogVersion = (\d+);", self.text).group(1))
        self.assertEqual(catalog.CATALOG_VERSION, v)

    def test_names(self):
        self.assertEqual(catalog.group_mask("arches"), 1 << 3)
        self.assertEqual(catalog.group_mask("arch,topper"), (1 << 3) | (1 << 2))
        self.assertEqual(catalog.group_mask("all"), 0xFFFF)
        self.assertEqual(catalog.effect_id("Leap"), 0x0200)
        self.assertEqual(catalog.effect_id("test-flash"), 0xFF00)
        self.assertEqual(catalog.effect_id("0x0123"), 0x0123)
        with self.assertRaises(ValueError):
            catalog.group_mask("reindeer")
        with self.assertRaises(ValueError):
            catalog.effect_id("disco")


if __name__ == "__main__":
    unittest.main()

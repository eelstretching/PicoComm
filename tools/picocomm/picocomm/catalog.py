"""Effect IDs and prop types: the Python mirror of include/picocomm/effects.h.

tests/test_catalog.py checks these tables against the header, so a new effect added there and
not here fails the tests.
"""

CATALOG_VERSION = 1

PROP_TYPES = {
    "unknown": 0,
    "megatree": 1,
    "topper": 2,
    "arch": 3,
    "minitree": 4,
    "bench": 15,
}

EFFECTS = {
    "none": 0x0000,
    "solidcolor": 0x0001,
    "starlight": 0x0002,
    "twinklefox": 0x0003,
    "fillwipe": 0x0004,
    "chase": 0x0005,
    "topperstarlight": 0x0100,
    "leap": 0x0200,
    "archrise": 0x0201,
    "cylon": 0x0202,
    "testflash": 0xFF00,
}

ALL_GROUPS = 0xFFFF

# Friendly plurals for the command line: --group arches.
_GROUP_ALIASES = {
    "all": None,
    "arches": "arch",
    "trees": "megatree",
    "tree": "megatree",
    "minitrees": "minitree",
    "toppers": "topper",
}


def prop_name(prop_type: int) -> str:
    for name, value in PROP_TYPES.items():
        if value == prop_type:
            return name
    return f"type{prop_type}"


def effect_name(effect: int) -> str:
    for name, value in EFFECTS.items():
        if value == effect:
            return name
    return f"0x{effect:04x}"


def group_mask(names: str) -> int:
    """'arches,topper' -> group bit mask. 'all' is every group."""
    mask = 0
    for raw in names.split(","):
        name = raw.strip().lower()
        if name in _GROUP_ALIASES:
            name = _GROUP_ALIASES[name]
            if name is None:
                return ALL_GROUPS
        if name not in PROP_TYPES:
            raise ValueError(f"unknown group {raw!r}; known: all, {', '.join(PROP_TYPES)}")
        mask |= 1 << PROP_TYPES[name]
    return mask


def effect_id(name: str) -> int:
    """'leap', or a number like 0x0200."""
    key = name.strip().lower().replace("_", "").replace("-", "")
    if key in EFFECTS:
        return EFFECTS[key]
    try:
        return int(name, 0)
    except ValueError:
        raise ValueError(f"unknown effect {name!r}; known: {', '.join(EFFECTS)}") from None

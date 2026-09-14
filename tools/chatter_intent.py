"""Bot intent system, milestone 1: gear/gold need DETECTION only.

Scope boundary, deliberate: this module only notices needs and
writes them to llm_bot_intents. It does NOT:
  - call .llmc ahlist/ahbuy (that's chatter_soap.call_soap_command,
    wired up by a future milestone once this detection pass has
    been observed running for a while)
  - feed intent context into chat prompts (also future work)
  - touch llm_chatter_bridge.py's main loop at all

Consumables/materials needs are explicitly out of scope for this
pass — vendor-sourcing already covers those via mod-playerbots'
own `maintenance` behavior, no new mechanics needed (see this
session's design conversation). Only gear and gold are handled
here.

Run this as a standalone periodic job (e.g. a cron-style call
from whatever schedules milestone-2 wiring) — it is intentionally
NOT imported by llm_chatter_bridge.py yet.
"""

import json
import logging
from typing import Optional

logger = logging.getLogger(__name__)

# --- Equipment slot IDs (WotLK 3.3.5a client, EquipmentSlots enum) ---
# Standard, stable across the whole AzerothCore/TrinityCore
# lineage — not expected to vary by fork.
EQUIPMENT_SLOT_MAINHAND = 15

# --- Cheap heuristics, deliberately rough (see design conversation:
# "cheap heuristic is fine") — not meant to model WotLK's actual
# non-linear ilvl-vs-level curve precisely, just catch gear that's
# obviously fallen behind. Both tunable via config so they don't
# need a code change to retune.
DEFAULT_GEAR_LEVEL_RATIO = 1.5
DEFAULT_GOLD_COPPER_PER_LEVEL = 1000  # 10 silver/level


def _config_float(config: dict, key: str, default: float) -> float:
    try:
        return float(config.get(key, default))
    except (TypeError, ValueError):
        return default


def get_always_on_guild_ids(config: dict) -> list:
    """Parse LLMChatter.GuildChatter.AlwaysOnGuildIds (the same
    config key LLMChatterWorld.cpp's CheckGuildIdleChatter()
    reads on the C++ side) into a list of ints. Self-contained
    here rather than importing llm_chatter_bridge's private
    _always_on_guilds_configured() (that one only returns a
    bool, and importing a leading-underscore helper across
    modules is a smell worth avoiding anyway).
    """
    raw = str(
        config.get(
            "LLMChatter.GuildChatter.AlwaysOnGuildIds", ""
        )
    ).strip()
    if not raw:
        return []
    ids = []
    for part in raw.split(","):
        part = part.strip()
        if part.isdigit():
            ids.append(int(part))
    return ids


def get_cast_roster(db, always_on_guild_ids) -> list:
    """Bots in the always-on narrative-cast guilds — same
    membership set LLMChatterWorld.cpp's CheckGuildIdleChatter()
    treats as active, just resolved from the Python/DB side via
    guild_member instead of the live in-game session list (this
    detection pass has no reason to require a bot be online right
    now — unlike the .llmc ahlist/ahbuy execution commands, which
    are online-only by design, a gear/gold check is a pure data
    read and works fine against an offline character's own row).
    """
    if not always_on_guild_ids:
        return []

    placeholders = ", ".join(
        ["%s"] * len(always_on_guild_ids)
    )
    cursor = db.cursor(dictionary=True)
    cursor.execute(
        f"""
        SELECT c.guid, c.name, c.level, c.money
        FROM guild_member gm
        JOIN characters c ON c.guid = gm.guid
        WHERE gm.guildid IN ({placeholders})
        """,
        tuple(always_on_guild_ids),
    )
    rows = cursor.fetchall()
    cursor.close()
    return rows


def _get_equipped_weapon_ilvl(
    db, bot_guid: int
) -> Optional[dict]:
    """Return {'item_entry', 'item_level'} for the bot's
    equipped main-hand weapon, or None if nothing is equipped
    in that slot. Cross-database join (acore_world.item_template)
    on the same connection/user — same pattern already used
    elsewhere in this module for auth-DB joins.
    """
    cursor = db.cursor(dictionary=True)
    cursor.execute(
        """
        SELECT ii.itemEntry AS item_entry,
               it.ItemLevel AS item_level
        FROM character_inventory ci
        JOIN item_instance ii ON ii.guid = ci.item
        JOIN acore_world.item_template it
            ON it.entry = ii.itemEntry
        WHERE ci.guid = %s
          AND ci.bag = 0
          AND ci.slot = %s
        LIMIT 1
        """,
        (bot_guid, EQUIPMENT_SLOT_MAINHAND),
    )
    row = cursor.fetchone()
    cursor.close()
    return row


def _has_open_intent(db, bot_guid: int, need_type: str) -> bool:
    cursor = db.cursor()
    cursor.execute(
        """
        SELECT 1 FROM llm_bot_intents
        WHERE bot_guid = %s AND need_type = %s
          AND status = 'open'
        LIMIT 1
        """,
        (bot_guid, need_type),
    )
    exists = cursor.fetchone() is not None
    cursor.close()
    return exists


def _upsert_intent(
    db, bot_guid: int, need_type: str, context: dict
):
    cursor = db.cursor()
    cursor.execute(
        """
        INSERT INTO llm_bot_intents
            (bot_guid, need_type, context, status)
        VALUES (%s, %s, %s, 'open')
        ON DUPLICATE KEY UPDATE
            context = VALUES(context),
            status = 'open'
        """,
        (bot_guid, need_type, json.dumps(context)),
    )
    db.commit()
    cursor.close()
    logger.info(
        "[intent] %s need_type=%s context=%s",
        bot_guid, need_type, context,
    )


def check_gear_need(db, config: dict, bot: dict) -> None:
    """Flag a gear need when the equipped weapon's item level
    is meaningfully behind the character's level, or when
    nothing is equipped in that slot at all. Rough heuristic by
    design — see module header.
    """
    if _has_open_intent(db, bot["guid"], "gear"):
        return

    ratio = _config_float(
        config,
        "LLMChatter.Intent.GearLevelRatio",
        DEFAULT_GEAR_LEVEL_RATIO,
    )
    level = bot["level"]
    weapon = _get_equipped_weapon_ilvl(db, bot["guid"])

    if weapon is None:
        _upsert_intent(
            db, bot["guid"], "gear",
            {
                "slot": EQUIPMENT_SLOT_MAINHAND,
                "current_item_entry": None,
                "current_item_level": 0,
                "character_level": level,
                "reason": "no weapon equipped",
            },
        )
        return

    expected_min_ilvl = level * ratio
    if weapon["item_level"] < expected_min_ilvl:
        _upsert_intent(
            db, bot["guid"], "gear",
            {
                "slot": EQUIPMENT_SLOT_MAINHAND,
                "current_item_entry": weapon["item_entry"],
                "current_item_level": weapon["item_level"],
                "character_level": level,
                "reason": "item level below "
                          f"{ratio}x character level",
            },
        )


def check_gold_need(db, config: dict, bot: dict) -> None:
    """Flag a gold need when copper on hand is below a rough
    per-level baseline (training/mount/repair costs all scale
    with level, so a flat threshold would be wrong at level 5
    vs. level 55 — see module header for why this is still
    deliberately approximate rather than modeling real costs).
    """
    if _has_open_intent(db, bot["guid"], "gold"):
        return

    copper_per_level = _config_float(
        config,
        "LLMChatter.Intent.GoldCopperPerLevel",
        DEFAULT_GOLD_COPPER_PER_LEVEL,
    )
    threshold = bot["level"] * copper_per_level
    if bot["money"] < threshold:
        _upsert_intent(
            db, bot["guid"], "gold",
            {
                "copper_on_hand": bot["money"],
                "threshold_copper": threshold,
                "character_level": bot["level"],
            },
        )


def run_intent_checks(
    db, config: dict, always_on_guild_ids=None
) -> int:
    """Run both checks for every cast-guild bot. Returns the
    number of bots evaluated. Safe to call repeatedly — both
    checks are no-ops for a bot that already has an open
    intent of that type, so this can run on any cadence without
    spamming duplicate rows.
    """
    if always_on_guild_ids is None:
        always_on_guild_ids = get_always_on_guild_ids(config)
    roster = get_cast_roster(db, always_on_guild_ids)
    for bot in roster:
        check_gear_need(db, config, bot)
        check_gold_need(db, config, bot)
    return len(roster)

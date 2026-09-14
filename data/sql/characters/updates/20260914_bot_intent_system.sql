-- --------------------------------------------------------
-- Bot intent system, milestone 1: gear/gold need detection.
--
-- Deliberately narrow scope for this first pass — see
-- tools/chatter_intent.py's module docstring. Detection only,
-- not yet wired into the bridge's main loop or into actually
-- calling .llmc ahlist/ahbuy.
--
-- Composite PK (bot_guid, need_type), not bot_guid alone — a
-- bot can plausibly need both gear AND gold at the same time,
-- these shouldn't collide.
-- --------------------------------------------------------

CREATE TABLE IF NOT EXISTS `llm_bot_intents` (
    `bot_guid`   INT UNSIGNED NOT NULL,
    `need_type`  ENUM('gear', 'gold') NOT NULL,
    -- gear: {"slot": <equip slot id>, "current_item_entry": N,
    --        "current_item_level": N, "character_level": N}
    -- gold: {"copper_on_hand": N, "threshold_copper": N}
    `context`    JSON NOT NULL,
    `status`     ENUM('open', 'resolved', 'dismissed')
                 NOT NULL DEFAULT 'open',
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
                 ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`bot_guid`, `need_type`),
    INDEX `idx_status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

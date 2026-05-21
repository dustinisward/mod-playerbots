-- #########################################################
-- Playerbots - PROJECT_GOALS pillar 3 combat-chat templates
-- Companion to PlayerbotAI::CombatChatOn{PullStarted,
-- WipeDetected,BossKilled} hooks (2026-05-20).
-- Gated at runtime by AiPlayerbot.RandomBotCombatCallouts.
-- enUS only — WoWZoW is enUS-only per project memory.
-- #########################################################

DELETE FROM ai_playerbot_texts WHERE name IN (
    'combat_pull_started',
    'combat_wipe_detected',
    'combat_boss_down'
);
DELETE FROM ai_playerbot_texts_chance WHERE name IN (
    'combat_pull_started',
    'combat_wipe_detected',
    'combat_boss_down'
);

-- combat_pull_started: fires when tank's pull resolves.
-- Placeholders: %target_name = pulled unit.
INSERT INTO `ai_playerbot_texts`
    (`id`, `name`, `text`, `say_type`, `reply_type`)
VALUES
    (1800, 'combat_pull_started', 'Pulling!',           0, 0),
    (1801, 'combat_pull_started', 'Incoming!',          0, 0),
    (1802, 'combat_pull_started', 'On %target_name!',   0, 0),
    (1803, 'combat_pull_started', 'Engage!',            0, 0);

INSERT INTO ai_playerbot_texts_chance (name, probability) VALUES ('combat_pull_started', 100);

-- combat_wipe_detected: fires once per wipe cycle.
-- Placeholders: %boss_name = last-engaged target (may be empty).
INSERT INTO `ai_playerbot_texts`
    (`id`, `name`, `text`, `say_type`, `reply_type`)
VALUES
    (1804, 'combat_wipe_detected', 'We''re wiped.',           0, 0),
    (1805, 'combat_wipe_detected', 'Regrouping.',             0, 0),
    (1806, 'combat_wipe_detected', 'Reset, bring it back.',   0, 0);

INSERT INTO ai_playerbot_texts_chance (name, probability) VALUES ('combat_wipe_detected', 100);

-- combat_boss_down: fires on IsDungeonBoss / isWorldBoss kill.
-- Placeholders: %boss_name = the slain boss.
INSERT INTO `ai_playerbot_texts`
    (`id`, `name`, `text`, `say_type`, `reply_type`)
VALUES
    (1807, 'combat_boss_down', 'Boss down!',        0, 0),
    (1808, 'combat_boss_down', 'Victory!',          0, 0),
    (1809, 'combat_boss_down', '%boss_name dies.',  0, 0),
    (1810, 'combat_boss_down', 'GG.',               0, 0);

INSERT INTO ai_playerbot_texts_chance (name, probability) VALUES ('combat_boss_down', 100);

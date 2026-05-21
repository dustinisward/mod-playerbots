/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// PROJECT_GOALS pillar 3 (2026-05-20): deterministic wipe-detection callout.
// Polls the bot's group; if every member (player + bot) is dead, fires
// `PlayerbotAI::CombatChatOnWipeDetected()` once per wipe cycle.
// Cooldown enforced inside CombatChatOnWipeDetected via m_combatEventCooldowns
// (keyed by group GUID XOR boss GUID), so this action is safe to poll every
// tick the host strategy enables it.

#ifndef _PLAYERBOT_WIPEDETECTIONACTION_H
#define _PLAYERBOT_WIPEDETECTIONACTION_H

#include "Action.h"

class PlayerbotAI;

class WipeDetectionAction : public Action
{
public:
    WipeDetectionAction(PlayerbotAI* botAI, std::string const name = "wipe chat")
        : Action(botAI, name) {}

    bool Execute(Event event) override;

    // Cheap early-out: only proceed when bot is dead and is grouped.
    // Heavier "all members dead" check happens inside Execute().
    bool isUseful() override;
};

#endif

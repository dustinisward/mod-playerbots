/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "WipeDetectionAction.h"

#include "Group.h"
#include "Player.h"
#include "Playerbots.h"

namespace
{
// True if every member of `group` is currently dead. Empty/null group is
// not "wiped" — we deliberately return false so isolated bots never trigger.
bool IsGroupFullyWiped(Group* group)
{
    if (!group)
        return false;

    bool sawAnyone = false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        sawAnyone = true;
        if (member->IsAlive())
            return false;
    }
    return sawAnyone;
}
}  // namespace

bool WipeDetectionAction::isUseful()
{
    // Cheap pre-checks: must be in a group, must be dead ourselves.
    // The full "every member dead" check happens in Execute() so this
    // poll stays fast on the hot path.
    if (!bot || bot->IsAlive())
        return false;
    if (!bot->GetGroup())
        return false;
    if (!sPlayerbotAIConfig.randomBotCombatCallouts)
        return false;
    return true;
}

bool WipeDetectionAction::Execute(Event /*event*/)
{
    Group* group = bot->GetGroup();
    if (!IsGroupFullyWiped(group))
        return false;

    // Pick the last engaged target as the wipe-causer (may be null —
    // CombatChatOnWipeDetected handles that case).
    Unit* boss = AI_VALUE(Unit*, "current target");

    // Cooldown enforcement + chat dispatch lives in PlayerbotAI; this action
    // is just the trigger surface. Returns true only when the callout actually
    // fired (i.e. cooldown elapsed and SayToParty succeeded).
    return botAI->CombatChatOnWipeDetected(boss);
}

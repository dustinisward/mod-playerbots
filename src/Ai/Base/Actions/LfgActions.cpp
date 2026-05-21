/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgActions.h"

#include "AiFactory.h"
#include "ItemVisitors.h"
#include "LFGMgr.h"
#include "Opcodes.h"
#include "Playerbots.h"
#include "World.h"
#include "WorldPacket.h"
#include "RandomPlayerbotMgr.h"

using namespace lfg;

bool LfgJoinAction::Execute(Event event) { return JoinLFG(); }

uint32 LfgJoinAction::GetRoles()
{
    if (!RandomPlayerbotMgr::instance().IsRandomBot(bot))
    {
        if (botAI->IsTank(bot))
            return PLAYER_ROLE_TANK;
        if (botAI->IsHeal(bot))
            return PLAYER_ROLE_HEALER;
        else
            return PLAYER_ROLE_DAMAGE;
    }

    uint8 spec = AiFactory::GetPlayerSpecTab(bot);
    switch (bot->getClass())
    {
        case CLASS_DRUID:
            // #998 2026-05-19: Feral spec needs a TANK-vs-DPS sub-classification
            // because the Feral talent tab (spec==1) covers both Bear and Cat
            // builds. Prior code used HasAura(16931) [Thick Hide] which only
            // fires when bear-shifted -- bears just-logged-in / haven't-yet-
            // shifted got misclassified as DPS, causing LFG role-mask mismatch
            // and LFG_ROLECHECK_MISSING_ROLE silent rejects (B169 #1 cause).
            // New heuristic: check the trained spell book. Maul (6807) and
            // Lacerate (33745) are bear-only abilities; if either is on the
            // bot's spell list, the bot is bear-specced. Cat falls through to
            // DAMAGE. Thick Hide aura kept as fallback for legacy edge cases.
            if (spec == 2)
                return PLAYER_ROLE_HEALER;
            else if (spec == 1 && (bot->HasSpell(6807) || bot->HasSpell(33745) || bot->HasAura(16931) /* thick hide */))
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_PALADIN:
            if (spec == 1)
                return PLAYER_ROLE_TANK;
            else if (!spec)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_PRIEST:
            if (spec != 2)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_SHAMAN:
            if (spec == 2)
                return PLAYER_ROLE_HEALER;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_WARRIOR:
            if (spec == 2)
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;
        case CLASS_DEATH_KNIGHT:
            // WoWZoW #995 (2026-05-19): align with IsTank() at PlayerbotAI.cpp:2466. In
            // 3.3.5a, Blood (spec 0) is the canonical tank spec, but ANY DK currently in
            // Frost Presence (aura 48263) is actively tanking and should queue as TANK
            // regardless of spec tab. Previously Frost/Unholy DKs in Frost Presence were
            // forced to DPS — wasting tank potential and contradicting IsTank semantics.
            if (spec == 0 || bot->HasAura(48263) /* SPELL_DK_FROST_PRESENCE */)
                return PLAYER_ROLE_TANK;
            else
                return PLAYER_ROLE_DAMAGE;
            break;

        default:
            return PLAYER_ROLE_DAMAGE;
            break;
    }

    return PLAYER_ROLE_DAMAGE;
}

// #1020 (2026-05-20): added observability for the 4 silent-return paths
// in JoinLFG. Prior to this patch, when a bot bailed early (already in
// LFG, no dungeons in team pool, no level-matched dungeons, list empty)
// nothing was logged. RDF stagnation triage couldn't see WHY a bot
// failed to queue. LOG_DEBUG (not INFO) because these fire per bot per
// queue-attempt tick -- at 600 bots * 30s ticks that's 1200/min of
// noise if at INFO. Owner can elevate to INFO via per-logger config.
bool LfgJoinAction::JoinLFG()
{
    // check if already in lfg
    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
    {
        LOG_DEBUG("playerbots", "Bot {} JoinLFG skip: already in LFG (state={})",
                  bot->GetName().c_str(), uint32(state));
        return false;
    }

    /*ItemCountByQuality visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_EQUIP);
    bool random = urand(0, 100) < 20;
    bool heroic = urand(0, 100) < 50 &&
                  (visitor.count[ITEM_QUALITY_EPIC] >= 3 || visitor.count[ITEM_QUALITY_RARE] >= 10) &&
                  bot->GetLevel() >= 70;
    bool rbotAId = !heroic && (urand(0, 100) < 50 && visitor.count[ITEM_QUALITY_EPIC] >= 5 &&
                               (bot->GetLevel() == 60 || bot->GetLevel() == 70 || bot->GetLevel() == 80));*/

    LfgDungeonSet list;
    std::vector<uint32> selected;

    std::vector<uint32> dungeons = RandomPlayerbotMgr::instance().LfgDungeons[bot->GetTeamId()];
    if (!dungeons.size())
    {
        LOG_DEBUG("playerbots",
                  "Bot {} JoinLFG skip: no dungeons configured for team {}",
                  bot->GetName().c_str(),
                  bot->GetTeamId() == TEAM_ALLIANCE ? "ALLIANCE" : "HORDE");
        return false;
    }

    for (std::vector<uint32>::iterator i = dungeons.begin(); i != dungeons.end(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*i);
        if (!dungeon || (dungeon->TypeID != LFG_TYPE_RANDOM && dungeon->TypeID != LFG_TYPE_DUNGEON &&
                         dungeon->TypeID != LFG_TYPE_HEROIC && dungeon->TypeID != LFG_TYPE_RAID))
            continue;

        auto const& botLevel = bot->GetLevel();

        // WoWZoW: honor DBC MinLevel..MaxLevel only. Upstream's `MinLevel + 10` cap on
        // LFG_TYPE_DUNGEON blocked L80 bots from filling level-scaled dungeons (Stratholme
        // MaxLevel=100, etc.) and from filling RDF queues for low-level humans. MaxLevel
        // from the DBC is the authoritative upper bound.
        if (dungeon->MinLevel && (botLevel < dungeon->MinLevel || botLevel > dungeon->MaxLevel))
            continue;

        selected.push_back(dungeon->ID);
        list.insert(dungeon->ID);
    }

    if (!selected.size())
    {
        LOG_DEBUG("playerbots",
                  "Bot {} JoinLFG skip: no level-matched dungeons (level={}, team pool size={})",
                  bot->GetName().c_str(), bot->GetLevel(), uint32(dungeons.size()));
        return false;
    }

    if (list.empty())
    {
        LOG_DEBUG("playerbots", "Bot {} JoinLFG skip: dungeon list empty after filter",
                  bot->GetName().c_str());
        return false;
    }

    bool many = list.size() > 1;
    LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(*list.begin());

    // check role for console msg
    std::string _roles = "multiple roles";
    uint32 roleMask = GetRoles();
    if (roleMask & PLAYER_ROLE_TANK)
        _roles = "TANK";

    if (roleMask & PLAYER_ROLE_HEALER)
        _roles = "HEAL";

    if (roleMask & PLAYER_ROLE_DAMAGE)
        _roles = "DPS";

    LOG_INFO("playerbots", "Bot {} {}:{} <{}>: queues LFG, Dungeon as {} ({})", bot->GetGUID().ToString().c_str(),
             bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str(), _roles,
             many ? "several dungeons" : dungeon->Name[0]);

    // Set RbotAId Browser comment
    std::string const _gs = std::to_string(botAI->GetEquipGearScore(bot/*, false, false*/));

    // JoinLfg is not threadsafe, so make packet and queue into session
    // sLFGMgr->JoinLfg(bot, roleMask, list, _gs);

    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << (uint32)roleMask;
    *data << (bool)false;
    *data << (bool)false;
    // Slots
    *data << (uint8)(list.size());
    for (uint32 dungeon : list)
        *data << (uint32)dungeon;
    // Needs
    *data << (uint8)3 << (uint8)0 << (uint8)0 << (uint8)0;
    *data << _gs;
    bot->GetSession()->QueuePacket(data);

    return true;
}

// #1004 (2026-05-20): idempotency fix. SMSG_LFG_ROLE_CHECK_UPDATE fires
// every time ANY party member changes role, so without an early-return
// each bot re-sends CMSG_LFG_SET_ROLES with the same role on every
// broadcast. With N bots in a group, role-check completion costs O(N^2)
// packet round-trips instead of O(N). The early-return was commented out
// pre-#1004 -- likely from a debug session where it was suspected to
// cause role-check stalls. Empirically, sLFGMgr->GetRoles() returns 0
// before any CMSG_LFG_SET_ROLES is processed, so first-fire always sends
// (currentRoles=0, newRoles=non-zero, 0!=non-zero, send). Subsequent
// fires after the bot's role is registered correctly skip. Re-enabled
// + added DEBUG log for skip path so observability still works.
bool LfgRoleCheckAction::Execute(Event /*event*/)
{
    if (Group* group = bot->GetGroup())
    {
        uint32 currentRoles = sLFGMgr->GetRoles(bot->GetGUID());
        uint32 newRoles = GetRoles();
        if (currentRoles == newRoles && currentRoles != 0)
        {
            LOG_DEBUG("playerbots", "Bot {} LFG role-check idempotent skip (roles={})",
                      bot->GetName().c_str(), newRoles);
            return false;
        }

        WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_ROLES);
        *packet << (uint8)newRoles;
        bot->GetSession()->QueuePacket(packet);
        // sLFGMgr->SetRoles(bot->GetGUID(), newRoles);
        // sLFGMgr->UpdateRoleCheck(group->GetGUID(), bot->GetGUID(), newRoles);

        LOG_INFO("playerbots", "Bot {} {}:{} <{}>: LFG roles checked", bot->GetGUID().ToString().c_str(),
                 bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

        return true;
    }

    return false;
}

// #1011 (2026-05-20): added observability for proposal-reject path. The
// in-combat / dead branches silently sent a NACK CMSG_LFG_PROPOSAL_RESULT
// pre-patch -- no log signal. RDF stagnation triage couldn't tell whether
// a bot rejected the proposal vs. never received it. New logging:
//   - LOG_INFO on proposal REJECT (in-combat / dead): includes reason
//   - LOG_INFO on proposal ACCEPT (stored or packet path)
// Both paths use INFO since proposals are infrequent (one per match-found
// event, not per tick) so log volume is bounded.
bool LfgAcceptAction::Execute(Event event)
{
    uint32 id = AI_VALUE(uint32, "lfg proposal");

    // Try accept if already stored
    if (id)
    {
        if (bot->IsInCombat() || bot->isDead())
        {
            LOG_INFO("playerbots",
                     "Bot {} LFG proposal REJECT id={} (stored path): {}",
                     bot->GetName().c_str(), id,
                     bot->IsInCombat() ? "in combat" : "dead");
            WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
            *packet << id << false;
            bot->GetSession()->QueuePacket(packet);
            return true;
        }

        LOG_INFO("playerbots", "Bot {} LFG proposal ACCEPT id={} (stored path)",
                 bot->GetName().c_str(), id);

        botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
        bot->ClearUnitState(UNIT_STATE_ALL_STATE);

        WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
        *packet << id << true;
        bot->GetSession()->QueuePacket(packet);

        if (RandomPlayerbotMgr::instance().IsRandomBot(bot) && !bot->GetGroup())
        {
            RandomPlayerbotMgr::instance().Refresh(bot);
            botAI->ResetStrategies();
        }

        botAI->Reset();
        return true;
    }

    // If we get the proposal packet, accept immediately
    if (!event.getPacket().empty())
    {
        WorldPacket p(event.getPacket());
        uint32 dungeonId;
        uint8 state;
        p >> dungeonId >> state >> id;

        if (id)
        {
            if (bot->IsInCombat() || bot->isDead())
            {
                LOG_INFO("playerbots",
                         "Bot {} LFG proposal REJECT id={} dungeon={} "
                         "(packet path): {}",
                         bot->GetName().c_str(), id, dungeonId,
                         bot->IsInCombat() ? "in combat" : "dead");
                WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
                *packet << id << false;
                bot->GetSession()->QueuePacket(packet);
                return true;
            }

            LOG_INFO("playerbots", "Bot {} LFG proposal ACCEPT id={} dungeon={} (packet path)",
                     bot->GetName().c_str(), id, dungeonId);

            botAI->GetAiObjectContext()->GetValue<uint32>("lfg proposal")->Set(0);
            bot->ClearUnitState(UNIT_STATE_ALL_STATE);

            WorldPacket* packet = new WorldPacket(CMSG_LFG_PROPOSAL_RESULT);
            *packet << id << true;
            bot->GetSession()->QueuePacket(packet);

            if (RandomPlayerbotMgr::instance().IsRandomBot(bot) && !bot->GetGroup())
            {
                RandomPlayerbotMgr::instance().Refresh(bot);
                botAI->ResetStrategies();
            }

            botAI->Reset();
            return true;
        }
    }

    return false;
}

// #1010 (2026-05-20): added observability. Prior to this patch, LfgLeaveAction
// silently returned false when the bot was already past LFG_STATE_QUEUED
// (in dungeon, in proposal, etc) AND silently sent CMSG_LFG_LEAVE on the
// happy path -- no log signal either way. With this patch the no-leave
// path emits LOG_DEBUG (no spam at 600-bot scale) and the actual-leave
// path emits LOG_INFO so log-grep can correlate bot-leaves-queue events
// with the SMSG_LFG_UPDATE_PLAYER REMOVED_FROM_QUEUE follow-up.
bool LfgLeaveAction::Execute(Event /*event*/)
{
    // Don't leave if lfg strategy enabled
    // if (botAI->HasStrategy("lfg", BOT_STATE_NON_COMBAT))
    //    return false;

    // Don't leave if already invited / in dungeon
    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state > LFG_STATE_QUEUED)
    {
        LOG_DEBUG("playerbots",
                  "Bot {} LFG leave SKIP (state={} > LFG_STATE_QUEUED): "
                  "already past queue, won't leave",
                  bot->GetName().c_str(), uint32(state));
        return false;
    }

    LOG_INFO("playerbots", "Bot {} LFG leave: sending CMSG_LFG_LEAVE (state={})",
             bot->GetName().c_str(), uint32(state));

    WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
    bot->GetSession()->QueuePacket(packet);
    // sLFGMgr->LeaveLfg(bot->GetGUID());
    return true;
}

bool LfgLeaveAction::isUseful() { return true; }

// P1 #994 (2026-05-19): bot vote-kick handler. Fires when the server emits
// SMSG_LFG_BOOT_PROPOSAL_UPDATE (a human-initiated kick proposal). The bot
// always-accepts so the human master can recruit votes from the bot party.
// Server enforces LFG.MaxKickCount + LFG.KickPreventionTimer so this can't
// be abused; and the victim's own self-vote is ignored by LFGMgr::UpdateBoot,
// so a bot being kicked won't block its own removal by voting no.
//
// Per RDF_DEEP_IMPROVEMENT_PLAN_2026_05_19.md §5 P1 -- LOW risk source
// patch; activates on next worldserver rebuild (build #19+). Mirrors the
// existing LfgAcceptAction pattern: build the canonical CMSG_*, queue
// via session, thread-safe per PR #1143's simulated-packet pattern.
bool LfgVoteKickAction::Execute(Event /*event*/)
{
    // Must be in a group to vote on a boot.
    if (!bot->GetGroup())
        return false;

    // Send CMSG_LFG_SET_BOOT_VOTE with agree=true. Per LFGHandler.cpp:133
    // the wire format is a single bool (1 byte). No proposal-id needed --
    // the server tracks boot state per-group internally via BootsStore[gguid]
    // and resolves the vote against the calling player's guid.
    WorldPacket* packet = new WorldPacket(CMSG_LFG_SET_BOOT_VOTE, 1);
    *packet << uint8(1);  // agree
    bot->GetSession()->QueuePacket(packet);
    return true;
}

bool LfgTeleportAction::Execute(Event event)
{
    bool out = false;

    WorldPacket p(event.getPacket());
    if (!p.empty())
    {
        p.rpos(0);
        p >> out;
    }

    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    WorldPacket* packet = new WorldPacket(CMSG_LFG_TELEPORT);
    *packet << out;
    bot->GetSession()->QueuePacket(packet);
    // sLFGMgr->TeleportPlayer(bot, out);

    return true;
}

// #1000 (2026-05-20): SMSG_LFG_TELEPORT_DENIED handler -- decode the reason
// code per LFGMgr's LfgTeleportError enum and log at INFO so dungeon-form
// telemetry has signal on stuck-bot scenarios. The packet wire format is a
// single uint32 (per Opcodes.h:541 comment). Reason codes from LFGMgr.h:
//   LFG_TELEPORTERROR_PLAYER_DEAD       = 1
//   LFG_TELEPORTERROR_FALLING           = 2
//   LFG_TELEPORTERROR_IN_VEHICLE        = 3 (3.3.5a observation; varies)
//   LFG_TELEPORTERROR_FATIGUE           = 4
//   LFG_TELEPORTERROR_INVALID_LOCATION  = 6
//   LFG_TELEPORTERROR_COMBAT            = 8
//   LFG_TELEPORTERROR_DAILY_DONE        = 9 (not commonly seen)
// Behavior: log + ClearUnitState (defensive). The bot does NOT immediately
// re-send CMSG_LFG_TELEPORT -- the server will re-prompt with another
// SMSG_LFG_TELEPORT once the blocking condition clears (combat ends,
// player resurrects, etc.) and the existing LfgTeleportAction handler
// will respond then.
bool LfgTeleportDeniedAction::Execute(Event event)
{
    uint32 reason = 0;
    WorldPacket p(event.getPacket());
    if (!p.empty())
    {
        p.rpos(0);
        p >> reason;
    }

    char const* reasonStr = "UNKNOWN";
    switch (reason)
    {
        case 1: reasonStr = "PLAYER_DEAD"; break;
        case 2: reasonStr = "FALLING"; break;
        case 3: reasonStr = "IN_VEHICLE"; break;
        case 4: reasonStr = "FATIGUE"; break;
        case 6: reasonStr = "INVALID_LOCATION"; break;
        case 8: reasonStr = "COMBAT"; break;
        case 9: reasonStr = "DAILY_DONE"; break;
        default: break;
    }

    LOG_INFO("playerbots", "Bot {} LFG teleport DENIED (reason {} {}): waiting for state clear",
             bot->GetName().c_str(), reason, reasonStr);

    // Defensive clear so a stale stun/disorient flag doesn't persist past
    // the denial. Same pattern as LfgTeleportAction::Execute.
    bot->ClearUnitState(UNIT_STATE_ALL_STATE);

    return true;
}

// #1002 (2026-05-20): SMSG_LFG_QUEUE_STATUS handler. Wire format per
// LFGHandler.cpp:471:
//   uint32 dungeonId
//   int32  waitTimeAvg     (server-wide average for this dungeon)
//   int32  waitTime        (this player's projected wait)
//   int32  waitTimeTank    (avg wait for TANK role specifically)
//   int32  waitTimeHealer  (avg wait for HEALER role)
//   int32  waitTimeDps     (avg wait for DPS role)
//   uint8  tanks           (STILL NEEDED to fill the group)
//   uint8  healers         (STILL NEEDED)
//   uint8  dps             (STILL NEEDED)
//   uint32 queuedTime      (how long THIS bot has been queued, sec)
// Rate-limited at the server side (~30s cadence per LFGMgr::SendUpdateStatus)
// so this won't spam logs. Pure observability -- no state mutation, no
// retry, no packet response. Just emit a single INFO line so log-grep
// can answer "is this bot's queue making progress?" without DB probes.
bool LfgQueueStatusAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    uint32 dungeonId = 0;
    int32 waitTimeAvg = 0, waitTime = 0, waitTimeTank = 0;
    int32 waitTimeHealer = 0, waitTimeDps = 0;
    uint8 tanks = 0, healers = 0, dps = 0;
    uint32 queuedTime = 0;

    p >> dungeonId >> waitTimeAvg >> waitTime >> waitTimeTank
      >> waitTimeHealer >> waitTimeDps >> tanks >> healers >> dps >> queuedTime;

    LOG_INFO("playerbots",
             "Bot {} LFG queue status: dungeon={} queuedFor={}s wait={}s "
             "avg={}s (T={}s H={}s D={}s) stillNeeded T={} H={} D={}",
             bot->GetName().c_str(), dungeonId, queuedTime, waitTime,
             waitTimeAvg, waitTimeTank, waitTimeHealer, waitTimeDps,
             tanks, healers, dps);

    return true;
}

// #1003 (2026-05-20): SMSG_LFG_JOIN_RESULT handler. Wire format per
// LFGHandler.cpp:458:
//   uint32 result   (LfgJoinResult enum, see LFGMgr.h:99-119)
//   uint32 state    (LfgRoleCheckState enum -- only meaningful when
//                    result == LFG_JOIN_FAILED for rolecheck failures)
//   [optional lockmap block -- skipped; not needed for observability]
// Decode result + state, log at INFO so log-grep can see why bots are
// failing to queue. LFG_JOIN_OK (0) is the success path and is logged
// at DEBUG (not INFO) to avoid spam at 600-bot scale.
bool LfgJoinResultAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    uint32 result = 0;
    uint32 state = 0;
    p >> result >> state;

    char const* resultStr = "UNKNOWN";
    switch (result)
    {
        case 0:  resultStr = "OK"; break;
        case 1:  resultStr = "FAILED_ROLECHECK"; break;
        case 2:  resultStr = "GROUPFULL"; break;
        case 4:  resultStr = "INTERNAL_ERROR"; break;
        case 5:  resultStr = "NOT_MEET_REQS"; break;
        case 6:  resultStr = "PARTY_NOT_MEET_REQS"; break;
        case 7:  resultStr = "MIXED_RAID_DUNGEON"; break;
        case 8:  resultStr = "MULTI_REALM"; break;
        case 9:  resultStr = "DISCONNECTED"; break;
        case 10: resultStr = "PARTY_INFO_FAILED"; break;
        case 11: resultStr = "DUNGEON_INVALID"; break;
        case 12: resultStr = "DESERTER"; break;
        case 13: resultStr = "PARTY_DESERTER"; break;
        case 14: resultStr = "RANDOM_COOLDOWN"; break;
        case 15: resultStr = "PARTY_RANDOM_COOLDOWN"; break;
        case 16: resultStr = "TOO_MUCH_MEMBERS"; break;
        case 17: resultStr = "USING_BG_SYSTEM"; break;
        default: break;
    }

    if (result == 0)
    {
        // Joined OK -- DEBUG only, 600-bot scale would spam INFO.
        LOG_DEBUG("playerbots", "Bot {} LFG join result: OK", bot->GetName().c_str());
    }
    else
    {
        LOG_INFO("playerbots", "Bot {} LFG join REJECTED (result {} {}, roleState {})",
                 bot->GetName().c_str(), result, resultStr, state);
    }

    return true;
}

// #1027 (2026-05-20): SMSG_LFG_ROLE_CHOSEN handler. Per-member rolecheck
// observability. Fires during the rolecheck phase as each member confirms
// their role. Wire format: ObjectGuid (8) + uint8 ready + uint32 roles.
// Logs at DEBUG (frequent during 5-member rolecheck, not INFO-worthy).
// Roles bitmask interpretation: 0x1=LEADER, 0x2=TANK, 0x4=HEALER, 0x8=DAMAGE.
bool LfgRoleChosenAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    ObjectGuid guid;
    uint8 ready = 0;
    uint32 roles = 0;
    p >> guid >> ready >> roles;

    LOG_DEBUG("playerbots",
              "Bot {} LFG role chosen: member={} ready={} roles=0x{:X}",
              bot->GetName().c_str(), guid.ToString(), uint32(ready), roles);

    return true;
}

// #1005 (2026-05-20): LfgUpdateType enum decode helper. Per LFG.h:48-63
// the LFG subsystem emits one of these 16 update types via UPDATE_PLAYER
// (self) and UPDATE_PARTY (broadcast to group). Returning a stable string
// here keeps the two Action implementations DRY.
static char const* LfgUpdateTypeName(uint8 updateType)
{
    switch (updateType)
    {
        case 0:  return "DEFAULT";
        case 1:  return "LEADER_UNK1";
        case 2:  return "LEAVE_RAIDBROWSER";
        case 3:  return "JOIN_RAIDBROWSER";
        case 4:  return "ROLECHECK_ABORTED";
        case 5:  return "JOIN_QUEUE";
        case 6:  return "ROLECHECK_FAILED";
        case 7:  return "REMOVED_FROM_QUEUE";
        case 8:  return "PROPOSAL_FAILED";
        case 9:  return "PROPOSAL_DECLINED";
        case 10: return "GROUP_FOUND";
        case 12: return "ADDED_TO_QUEUE";
        case 13: return "PROPOSAL_BEGIN";
        case 14: return "UPDATE_STATUS";
        case 15: return "GROUP_MEMBER_OFFLINE";
        case 16: return "GROUP_DISBAND";
        default: return "UNKNOWN";
    }
}

// #1005 SMSG_LFG_UPDATE_PLAYER handler. Wire format per LFGHandler.cpp:332:
//   uint8 updateType  (LfgUpdateType enum, see helper above)
//   uint8 hasData     (1 if more bytes follow)
//   [optional: queued, unk x3, dungeon count + ids, comment string]
// We only need updateType for observability -- the trailing data is
// useful for client UI but not for log signal.
bool LfgUpdatePlayerAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    uint8 updateType = 0;
    p >> updateType;

    LOG_INFO("playerbots", "Bot {} LFG update (player): {} ({})",
             bot->GetName().c_str(), updateType, LfgUpdateTypeName(updateType));

    return true;
}

// #1005 SMSG_LFG_UPDATE_PARTY handler. Wire format per LFGHandler.cpp:373:
//   uint8 updateType  (LfgUpdateType enum)
//   uint8 hasData     (1 if more bytes follow)
//   [optional: similar to UPDATE_PLAYER but with extra leader role byte]
// Same observability shape as UPDATE_PLAYER but tagged with "(party)" so
// log-grep can distinguish self-state vs. group-broadcast updates.
bool LfgUpdatePartyAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    uint8 updateType = 0;
    p >> updateType;

    LOG_INFO("playerbots", "Bot {} LFG update (party): {} ({})",
             bot->GetName().c_str(), updateType, LfgUpdateTypeName(updateType));

    return true;
}

// #1008 (2026-05-20): SMSG_LFG_PLAYER_REWARD handler. Wire format per
// LFGHandler.cpp:499:
//   uint32 rdungeonEntry  (random dungeon id, or 0 for specific queue)
//   uint32 sdungeonEntry  (specific dungeon that was run)
//   uint8  done           (1 = first-of-day reward, 0 = repeat run)
//   uint32 unused1 (always 1 in serverside)
//   uint32 money
//   uint32 xp
//   uint32 unused2 (always 0)
//   uint32 unused3 (always 0)
//   uint8  itemNum
//   [optional items array we skip]
// For observability we only need dungeon ids + done flag + money/xp.
// Logging at INFO so per-session completion telemetry is grep-able.
bool LfgPlayerRewardAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    if (p.empty())
        return false;

    p.rpos(0);
    uint32 rdungeonEntry = 0, sdungeonEntry = 0;
    uint8 done = 0;
    uint32 unused1 = 0, money = 0, xp = 0;
    p >> rdungeonEntry >> sdungeonEntry >> done >> unused1 >> money >> xp;

    LOG_INFO("playerbots",
             "Bot {} LFG dungeon COMPLETE: rdungeon={} sdungeon={} "
             "done={} money={} xp={}",
             bot->GetName().c_str(), rdungeonEntry, sdungeonEntry,
             done, money, xp);

    return true;
}

bool LfgJoinAction::isUseful()
{
    if (!sPlayerbotAIConfig.randomBotJoinLfg)
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
    }

    if (bot->GetLevel() < 15)
        return false;

    // don't use if active player master
    if (GET_PLAYERBOT_AI(bot)->IsRealPlayer())
        return false;

    if (bot->GetGroup() && bot->GetGroup()->GetLeaderGUID() != bot->GetGUID())
    {
        // botAI->ChangeStrategy("-lfg", BOT_STATE_NON_COMBAT);
        return false;
    }

    if (bot->IsBeingTeleported())
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (bot->isDead())
        return false;

    if (!RandomPlayerbotMgr::instance().IsRandomBot(bot))
        return false;

    Map* map = bot->GetMap();
    if (map && map->Instanceable())
        return false;

    LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != LFG_STATE_NONE)
        return false;

    return true;
}

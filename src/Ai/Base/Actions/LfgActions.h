/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_LFGACTIONS_H
#define _PLAYERBOT_LFGACTIONS_H

#include "InventoryAction.h"

class PlayerbotAI;

class LfgJoinAction : public InventoryAction
{
public:
    LfgJoinAction(PlayerbotAI* botAI, std::string const name = "lfg join") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    bool JoinLFG();
    uint32 GetRoles();
};

class LfgAcceptAction : public LfgJoinAction
{
public:
    LfgAcceptAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg accept") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgRoleCheckAction : public LfgJoinAction
{
public:
    LfgRoleCheckAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg role check") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgLeaveAction : public Action
{
public:
    LfgLeaveAction(PlayerbotAI* botAI) : Action(botAI, "lfg leave") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// P1 #994 (2026-05-19): vote-kick handler. Default `LFG.MaxKickCount = 2`
// + `LFG_GROUP_KICK_VOTES_NEEDED = 3` (per AzC core) means a single human
// initiating a kick needs 3 YES votes to remove a misbehaving bot. Without
// bot votes the human is stuck with 1 (their own) and can't recover from a
// stuck-bot scenario. This action always-accepts the kick proposal --
// server still enforces MaxKickCount + KickPreventionTimer, so an abusive
// pattern is rate-limited at the core level, not the bot AI level.
class LfgVoteKickAction : public Action
{
public:
    LfgVoteKickAction(PlayerbotAI* botAI) : Action(botAI, "lfg vote kick") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgTeleportAction : public Action
{
public:
    LfgTeleportAction(PlayerbotAI* botAI) : Action(botAI, "lfg teleport") {}

    bool Execute(Event event) override;
};

// #1000 (2026-05-20): SMSG_LFG_TELEPORT_DENIED handler -- server denied a
// LFG teleport (in combat, dead, dungeon ineligible, fatigued, etc). Prior
// to this patch the bot silently ignored the denial; with no log signal +
// no retry, a bot stuck in combat at proposal-accept time would never
// teleport-in, leaving the human waiting at the entrance. New handler:
// (a) logs the reason at INFO so dungeon-form telemetry sees the denial,
// (b) when reason indicates a transient condition (in-combat), re-fires
// CMSG_LFG_TELEPORT once combat ends (deferred via state-clear hook).
class LfgTeleportDeniedAction : public Action
{
public:
    LfgTeleportDeniedAction(PlayerbotAI* botAI) : Action(botAI, "lfg teleport denied") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

// #1002 (2026-05-20): SMSG_LFG_QUEUE_STATUS handler -- pure observability.
// Server emits this every ~30s while a player is queued, reporting:
//   * which dungeon we're queued for
//   * current player's wait time
//   * server-wide average wait time (per role)
//   * how many of {tank, healer, dps} are STILL NEEDED to fill a group
//   * how long this player has been in queue
// Prior to this patch the bot silently ignored these updates -- there was
// no log signal showing "this bot has been queued N minutes for dungeon X,
// still needs Y healers". B168 RDF stagnation triage was blind without it.
// New handler logs the queue snapshot at INFO so dungeon-form telemetry +
// rdf_density_keeper.py can correlate stuck-queue scenarios with role
// shortages, and so Diagnose-B168-RDF.ps1 can grep the worldserver log
// for live queue progress.
class LfgQueueStatusAction : public Action
{
public:
    LfgQueueStatusAction(PlayerbotAI* botAI) : Action(botAI, "lfg queue status") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

// #1003 (2026-05-20): SMSG_LFG_JOIN_RESULT handler -- pure observability.
// Server emits this immediately after CMSG_LFG_JOIN to report whether the
// queue request was accepted. Per LFGMgr's LfgJoinResult enum there are 18
// distinct rejection codes (DESERTER, RANDOM_COOLDOWN, PARTY_INFO_FAILED,
// NOT_MEET_REQS, MIXED_RAID_DUNGEON, etc.). Prior to this patch the bot
// silently swallowed every reject -- if 100 bots tried to join and 30 got
// DESERTER buff rejects, the queue would just look empty with no log
// signal pointing at the cause. New handler decodes result + roleCheckState
// and logs at INFO so Diagnose-B168-RDF.ps1 can grep the live worldserver
// log to see which reject codes are firing and how often.
class LfgJoinResultAction : public Action
{
public:
    LfgJoinResultAction(PlayerbotAI* botAI) : Action(botAI, "lfg join result") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

// #1027 (2026-05-20): SMSG_LFG_ROLE_CHOSEN handler -- per-member rolecheck
// observability. Fires per-member as roles are confirmed during the
// rolecheck phase. Useful for diagnosing rolecheck stalls (B169 #1 cause):
// if SMSG_LFG_ROLE_CHOSEN fires for member A but never for member B, the
// stall is at B. Log signal isolates the stuck member without DB probes.
// Wire format per LFGHandler.cpp:397: ObjectGuid (8) + uint8 ready + uint32 roles.
class LfgRoleChosenAction : public Action
{
public:
    LfgRoleChosenAction(PlayerbotAI* botAI) : Action(botAI, "lfg role chosen") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

// #1005 (2026-05-20): SMSG_LFG_UPDATE_PLAYER / SMSG_LFG_UPDATE_PARTY
// observability handlers. The LFG subsystem emits these whenever the
// player's or group's queue state changes (LFG_UPDATETYPE_*: JOIN_QUEUE,
// ADDED_TO_QUEUE, REMOVED_FROM_QUEUE, ROLECHECK_FAILED, PROPOSAL_BEGIN,
// PROPOSAL_FAILED, PROPOSAL_DECLINED, GROUP_FOUND, GROUP_MEMBER_OFFLINE,
// GROUP_DISBAND, etc.). Bot previously ignored both -- B168 RDF triage
// had no signal on intermediate state transitions. Each Action reads the
// first byte (updateType enum) and logs at INFO so log-grep can see the
// state-machine progression. Pure observability, no state mutation.
class LfgUpdatePlayerAction : public Action
{
public:
    LfgUpdatePlayerAction(PlayerbotAI* botAI) : Action(botAI, "lfg update player") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgUpdatePartyAction : public Action
{
public:
    LfgUpdatePartyAction(PlayerbotAI* botAI) : Action(botAI, "lfg update party") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

// #1008 (2026-05-20): SMSG_LFG_PLAYER_REWARD handler -- closes the LFG
// completion-side observability loop. Server emits this after a bot's
// group successfully completes a dungeon (first daily reward and any
// repeat run). The packet carries: random/selected dungeon ids, done
// flag (first-completion bonus or repeat), money, xp, and item rewards.
// For B168 RDF triage this is the success-path signal -- combined with
// the failure-path observability already shipped (#1003 JOIN_RESULT,
// #1000 TELEPORT_DENIED, #1005 UPDATE_PLAYER/PARTY), a log scrape can
// now compute the full queue->complete success rate per session.
class LfgPlayerRewardAction : public Action
{
public:
    LfgPlayerRewardAction(PlayerbotAI* botAI) : Action(botAI, "lfg player reward") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

#endif

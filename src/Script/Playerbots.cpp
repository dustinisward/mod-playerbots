/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Playerbots.h"

#include "BattlefieldScript.h"
#include "Channel.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "GuildTaskMgr.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotSpellRepository.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "PlayerbotCommandScript.h"
#include "cmath"
#include "BattleGroundTactics.h"

class PlayerbotsDatabaseScript : public DatabaseScript
{
public:
    PlayerbotsDatabaseScript() : DatabaseScript("PlayerbotsDatabaseScript") {}

    bool OnDatabasesLoading() override
    {
        DatabaseLoader playerbotLoader("server.playerbots");
        playerbotLoader.SetUpdateFlags(sConfigMgr->GetOption<bool>("Playerbots.Updates.EnableDatabases", true)
                                           ? DatabaseLoader::DATABASE_PLAYERBOTS
                                           : 0);
        playerbotLoader.AddDatabase(PlayerbotsDatabase, "Playerbots");

        return playerbotLoader.Load();
    }

    void OnDatabasesKeepAlive() override { PlayerbotsDatabase.KeepAlive(); }

    void OnDatabasesClosing() override { PlayerbotsDatabase.Close(); }

    void OnDatabaseWarnAboutSyncQueries(bool apply) override { PlayerbotsDatabase.WarnAboutSyncQueries(apply); }

    void OnDatabaseSelectIndexLogout(Player* player, uint32& statementIndex, uint32& statementParam) override
    {
        statementIndex = CHAR_UPD_CHAR_OFFLINE;
        statementParam = player->GetGUID().GetCounter();
    }

    void OnDatabaseGetDBRevision(std::string& revision) override
    {
        if (QueryResult resultPlayerbot =
                PlayerbotsDatabase.Query("SELECT date FROM version_db_playerbots ORDER BY date DESC LIMIT 1"))
        {
            Field* fields = resultPlayerbot->Fetch();
            revision = fields[0].Get<std::string>();
        }

        if (revision.empty())
            revision = "Unknown Playerbots Database Revision";
    }
};

class PlayerbotsPlayerScript : public PlayerScript
{
public:
    PlayerbotsPlayerScript() : PlayerScript("PlayerbotsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_AFTER_UPDATE,
        PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS,
        PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player->GetSession()->IsBot())
        {
            // B146 (2026-05-13): WoWZoW persistent morph overrides. Keep
            // race/faction/abilities/talents intact; only the rendered model
            // changes. Auto-applied at every login (no manual .morph typing).
            // Owner-requested for Sushh (guid=32892) -> Blood Elf Female body
            // while keeping Night Elf race + Alliance + NE racials. Hardcoded
            // map for now; future ship may move to a `wowzow_morph_overrides`
            // DB table for runtime adjustment.
            static const std::unordered_map<uint32, uint32> _wowzowMorphOverrides = {
                {32892u, 16115u},  // Sushh: NE Rogue keeps race, displays as BE Female (Magistrix Aminel base model)
            };
            uint32 charGuid = player->GetGUID().GetCounter();
            auto morphIt = _wowzowMorphOverrides.find(charGuid);
            if (morphIt != _wowzowMorphOverrides.end())
            {
                player->SetDisplayId(morphIt->second);
                player->SetNativeDisplayId(morphIt->second);
            }

            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);

            // Before modifying the following messages, please make sure it does not violate the AGPLv3.0 license
            // especially if you are distributing a repack or hosting a public server
            // e.g. you can replace the URL with your own repository,
            // but it should be publicly accessible and include all modifications you've made
            if (sPlayerbotAIConfig.enabled)
            {
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00This server runs with |cff00ccffmod-playerbots|r "
                    "|cffcccccchttps://github.com/mod-playerbots/mod-playerbots|r");
            }

            if (sPlayerbotAIConfig.enabled || sPlayerbotAIConfig.randomBotAutologin)
            {
                std::string maxAllowedBotCount = std::to_string(sRandomPlayerbotMgr.GetMaxAllowedBotCount());

                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00Playerbots:|r The server is configured with " + maxAllowedBotCount + " bots.");
            }
        }
    }

    bool OnPlayerBeforeTeleport(Player* /*player*/, uint32 /*mapid*/, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        /* for now commmented out until proven its actually required
        * havent seen any proof CleanVisibilityReferences() is needed

        // If the player is not safe to touch, do nothing
        if (!player)
            return true;

        // If same map or not in world do nothing
        if (!player->IsInWorld() || player->GetMapId() == mapid)
            return true;

        // If real player do nothing
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ai->IsRealPlayer())
            return true;

        // Cross-map bot teleport: defer visibility reference cleanup.
        // CleanVisibilityReferences() erases this bot's GUID from other objects' visibility containers.
        // This is intentionally done via the event queue (instead of directly here) because erasing
        // from other players' visibility maps inside the teleport call stack can hit unsafe re-entrancy
        // or iterator invalidation while visibility updates are in progress
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset(
            [guid, mapid]()
            {
                // do nothing, if the player is not safe to touch
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p || !p->IsInWorld() || p->IsDuringRemoveFromWorld())
                    return;

                // do nothing if we are already on the target map
                if (p->GetMapId() == mapid)
                    return;

                p->GetObjectVisibilityContainer().CleanVisibilityReferences();
            },
            Milliseconds(0));

        */

        return true;
    }

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
        {
            botAI->UpdateAI(diff);
        }

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            playerbotMgr->UpdateAI(diff);
        }
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER)
        {
            return true;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);

        if (botAI == nullptr)
        {
            return true;
        }

        botAI->HandleCommand(type, msg, player);

        // hotfix; otherwise the server will crash when whispering logout
        // https://github.com/mod-playerbots/mod-playerbots/pull/1838
        // TODO: find the root cause and solve it. (does not happen in party chat)
        if (msg == "logout")
            return false;

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* const member = itr->GetSource();

            if (member == nullptr)
                continue;

            PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);

            if (botAI == nullptr)
                continue;

            botAI->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* /*guild*/) override
    {
        if (type != CHAT_MSG_GUILD)
            return true;

        PlayerbotMgr* playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr == nullptr)
            return true;

        for (PlayerBotMap::const_iterator it = playerbotMgr->GetPlayerBotsBegin(); it != playerbotMgr->GetPlayerBotsEnd(); ++it)
        {
            Player* const bot = it->second;

            if (bot == nullptr)
                continue;

            if (bot->GetGuildId() != player->GetGuildId())
                continue;

            PlayerbotsMgr::instance().GetPlayerbotAI(bot)->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        PlayerbotMgr* const playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr != nullptr && channel->GetFlags() & 0x18)
            playerbotMgr->HandleCommand(type, msg);

        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        return true;
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if ((sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player)) &&
            (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL)))
        {
            return false;
        }

        return true;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        // early return
        if (sPlayerbotAIConfig.randomBotXPRate == 1.0 || !player)
            return;

        // no XP multiplier, when player is no bot.
        if (!player->GetSession()->IsBot() || !sRandomPlayerbotMgr.IsRandomBot(player))
            return;

        // no XP multiplier, when bot is in a group with a real player.
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (!member)
                    continue;

                if (!member->GetSession()->IsBot())
                    return;
            }
        }

        // otherwise apply bot XP multiplier.
        amount = static_cast<uint32>(std::round(static_cast<float>(amount) * sPlayerbotAIConfig.randomBotXPRate));
    }
};

class PlayerbotsMiscScript : public MiscScript
{
public:
    PlayerbotsMiscScript() : MiscScript("PlayerbotsMiscScript", {MISCHOOK_ON_DESTRUCT_PLAYER}) {}

    void OnDestructPlayer(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            delete botAI;

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            delete playerbotMgr;
    }
};

class PlayerbotsServerScript : public ServerScript
{
public:
    PlayerbotsServerScript() : ServerScript("PlayerbotsServerScript", {
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    void OnPacketReceived(WorldSession* session, WorldPacket const& packet) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->HandleMasterIncomingPacket(packet);
    }
};

class PlayerbotsWorldScript : public WorldScript
{
public:
    PlayerbotsWorldScript() : WorldScript("PlayerbotsWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnBeforeWorldInitialized() override
    {
        // Before modifying the following messages, please make sure it does not violate the AGPLv3.0 license
        // especially if you are distributing a repack or hosting a public server
        // e.g. you can replace the URL with your own repository,
        // but it should be publicly accessible and include all modifications you've made
        LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "║              AzerothCore Playerbots Module               ║");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     mod-playerbots is a community-driven open-source     ║");
        LOG_INFO("server.loading", "║  project based on AzerothCore, licensed under AGPLv3.0   ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║      https://github.com/mod-playerbots/mod-playerbots    ║");
        LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

        uint32 oldMSTime = getMSTime();

        LOG_INFO("server.loading", " ");
        LOG_INFO("server.loading", "Load Playerbots Config...");

        sPlayerbotAIConfig.Initialize();

        LOG_INFO("server.loading", ">> Loaded playerbots config in {} ms", GetMSTimeDiffToNow(oldMSTime));
        LOG_INFO("server.loading", " ");

        PlayerbotSpellRepository::Instance().Initialize();

        LOG_INFO("server.loading", "Playerbots World Thread Processor initialized");
    }

    void OnUpdate(uint32 diff) override
    {
        PlayerbotWorldThreadProcessor::instance().Update(diff);
        sRandomPlayerbotMgr.UpdateAI(diff);  // World thread only
    }
};

class PlayerbotsScript : public PlayerbotScript
{
public:
    PlayerbotsScript() : PlayerbotScript("PlayerbotsScript") {}

    // WoWZoW (Wishmaster #1521 thread-safe port via PR #1143 pattern, 2026-05-20):
    //   Robust LFG queue-eligibility check. Three changes over the old loop:
    //     1. Filter placeholder GUIDs (empty / non-Player / non-Group) so the
    //        very first LFG frame -- where the core has not yet populated the
    //        member list -- does not veto with a false negative (was producing
    //        "dungeon 0 / type 0" + LFG_ROLECHECK_MISSING_ROLE timeouts; see
    //        B169 trace path #1).
    //     2. Distinguish real players, bots, offline members, group-GUID
    //        sentinel. The hybrid policy preserves the old semantics
    //        (allow only with a real player / group GUID) for fully populated
    //        frames while letting placeholder-only frames slide.
    //     3. On detecting an offline member with otherwise-populated slots,
    //        soft-reset each bot's AI (clears "lfg proposal" / role-check) and
    //        return false to force a clean retry next tick. This is the inline
    //        equivalent of the "toggle bots off/on" workaround users found.
    //   Thread safety: this hook fires from LFGMgr::Update() on the world
    //   thread. botAI->Reset(true) mutates AI-local state only (no sLFGMgr
    //   touch). No new direct sLFGMgr->Join/Leave/SetRoles calls are added --
    //   PR #1143's simulated-packet pattern at LfgActions.cpp:200-211 remains
    //   the sole join path.
    bool OnPlayerbotCheckLFGQueue(lfg::Lfg5Guids const& guidsList) override
    {
        const size_t totalSlots = guidsList.guids.size();
        size_t ignoredEmpty = 0;
        size_t ignoredNonPlayer = 0;
        size_t offlinePlayers = 0;
        size_t botPlayers = 0;
        size_t realPlayers = 0;
        bool groupGuidSeen = false;

        LOG_DEBUG("playerbots", "[LFG] check start: slots={}", totalSlots);

        for (size_t i = 0; i < totalSlots; ++i)
        {
            ObjectGuid const& guid = guidsList.guids[i];

            // 1) Placeholders to ignore (early-frame pre-population artifacts).
            if (guid.IsEmpty())
            {
                ++ignoredEmpty;
                LOG_DEBUG("playerbots", "[LFG] slot {}: <empty> -> ignored", i);
                continue;
            }

            // Group GUID counts as "real player present" for compat with the
            // pre-port behavior (the old loop treated IsGroup() as non-bot).
            if (guid.IsGroup())
            {
                groupGuidSeen = true;
                LOG_DEBUG("playerbots",
                          "[LFG] slot {}: <GROUP GUID> -> counts as real-player (compat)", i);
                continue;
            }

            // Other non-Player GUIDs: pet/object/item placeholders, ignore.
            if (!guid.IsPlayer())
            {
                ++ignoredNonPlayer;
                LOG_DEBUG("playerbots",
                          "[LFG] slot {}: guid={} (non-player/high={}) -> ignored",
                          i,
                          static_cast<uint64>(guid.GetRawValue()),
                          (unsigned)guid.GetHigh());
                continue;
            }

            // 2) Player slot -- online?
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                ++offlinePlayers;
                LOG_DEBUG("playerbots",
                          "[LFG] slot {}: player guid={} offline/not-in-world",
                          i,
                          static_cast<uint64>(guid.GetRawValue()));
                continue;
            }

            // 3) Bot vs real player.
            if (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            {
                ++botPlayers;
                LOG_DEBUG("playerbots",
                          "[LFG] slot {}: BOT {} (lvl {}, class {})",
                          i,
                          player->GetName().c_str(),
                          player->GetLevel(),
                          player->getClass());
            }
            else
            {
                ++realPlayers;
                LOG_DEBUG("playerbots",
                          "[LFG] slot {}: REAL {} (lvl {}, class {})",
                          i,
                          player->GetName().c_str(),
                          player->GetLevel(),
                          player->getClass());
            }
        }

        // "Ultra-early phase" detection: no resolvable players AND every slot
        // was a placeholder. Do NOT veto -- let the core finish populating.
        const bool onlyPlaceholders =
            (realPlayers + botPlayers + (groupGuidSeen ? 1 : 0)) == 0 &&
            (ignoredEmpty + ignoredNonPlayer) == totalSlots;

        // Soft preflight: real members visible AND at least one offline.
        if (!onlyPlaceholders && offlinePlayers > 0)
        {
            // Prefer a real online player as leader-proxy; fall back to any
            // online player (bot or real).
            Player* leader = nullptr;

            for (ObjectGuid const& guid : guidsList.guids)
            {
                if (!guid.IsPlayer())
                    continue;
                if (Player* p = ObjectAccessor::FindPlayer(guid))
                {
                    if (PlayerbotsMgr::instance().GetPlayerbotAI(p) == nullptr)
                    {
                        leader = p;
                        break;
                    }
                }
            }

            if (!leader)
            {
                for (ObjectGuid const& guid : guidsList.guids)
                {
                    if (!guid.IsPlayer())
                        continue;
                    if (Player* p = ObjectAccessor::FindPlayer(guid))
                    {
                        leader = p;
                        break;
                    }
                }
            }

            if (leader)
            {
                Group* g = leader->GetGroup();
                if (g)
                {
                    LOG_DEBUG("playerbots",
                              "[LFG-RESET] group members={}, isRaid={}, isLFGGroup={}",
                              (int)g->GetMembersCount(),
                              g->isRaidGroup() ? 1 : 0,
                              g->isLFGGroup() ? 1 : 0);

                    // Soft reset of LFG-related AI state on every bot member.
                    // Reset(true) clears "lfg proposal" and resets engines; it
                    // does NOT call into sLFGMgr.
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (!member)
                            continue;

                        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(member))
                            ai->Reset(true);
                    }
                }
            }

            LOG_DEBUG("playerbots",
                      "[LFG] preflight soft-reset triggered (offline detected) -> allowQueue=no (retry)");
            return false;  // small deliberate retry after reset
        }

        // Hybrid policy: permissive in placeholder-only frames; otherwise the
        // original semantics (no offline AND at least one real player OR a
        // group GUID).
        bool allowQueue = onlyPlaceholders
                              ? true
                              : ((offlinePlayers == 0) && (realPlayers >= 1 || groupGuidSeen));

        LOG_DEBUG("playerbots",
                  "[LFG] summary: slots={}, real={}, bots={}, offline={}, "
                  "ignored(empty+nonPlayer)={}, groupGuidSeen={} -> allowQueue={}",
                  totalSlots,
                  realPlayers,
                  botPlayers,
                  offlinePlayers,
                  (ignoredEmpty + ignoredNonPlayer),
                  (groupGuidSeen ? "yes" : "no"),
                  (allowQueue ? "yes" : "no"));

        return allowQueue;
    }

    void OnPlayerbotCheckKillTask(Player* player, Unit* victim) override
    {
        if (player)
            GuildTaskMgr::instance().CheckKillTask(player, victim);
    }

    void OnPlayerbotCheckPetitionAccount(Player* player, bool& found) override
    {
        if (!found)
            return;

        if (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            found = false;
    }

    bool OnPlayerbotCheckUpdatesToSend(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI == nullptr)
            return true;

        return botAI->IsRealPlayer();
    }

    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (player == nullptr)
            return;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            botAI->HandleBotOutgoingPacket(*packet);

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            playerbotMgr->HandleMasterOutgoingPacket(*packet);
    }

    void OnPlayerbotUpdate(uint32 /*diff*/) override
    {
        sRandomPlayerbotMgr.UpdateSessions();  // Per-bot updates only
    }

    void OnPlayerbotUpdateSessions(Player* player) override
    {
        if (player)
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->UpdateSessions();
    }

    void OnPlayerbotLogout(Player* player) override
    {
        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

            if (botAI == nullptr || botAI->IsRealPlayer())
            {
                playerbotMgr->LogoutAllBots();
            }
        }

        sRandomPlayerbotMgr.OnPlayerLogout(player);
    }

    void OnPlayerbotLogoutBots() override
    {
        LOG_INFO("playerbots", "Logging out all bots...");
        sRandomPlayerbotMgr.LogoutAllBots();
    }
};

class PlayerBotsBGScript : public BGScript
{
public:
    PlayerBotsBGScript() : BGScript("PlayerBotsBGScript") {}

    void OnBattlegroundStart(Battleground* bg) override
    {
        BGStrategyData data;

        switch (bg->GetBgTypeID())
        {
            case BATTLEGROUND_WS:
                data.allianceStrategy = urand(0, WS_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, WS_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AB:
                data.allianceStrategy = urand(0, AB_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AB_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AV:
                data.allianceStrategy = urand(0, AV_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AV_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_EY:
                data.allianceStrategy = urand(0, EY_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, EY_STRATEGY_MAX - 1);
                break;
            default:
                break;
        }

        bgStrategies[bg->GetInstanceID()] = data;
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeam*/) override { bgStrategies.erase(bg->GetInstanceID()); }
};

// Workaround for missing InitEnabledHooksIfNeeded for new BattlefieldScript in ScriptMgr
class PlayerbotsBattlefieldScript : public BattlefieldScript
{
public:
    PlayerbotsBattlefieldScript() : BattlefieldScript("PlayerbotsBattlefieldScript") { }
};

void AddPlayerbotsSecureLoginScripts();

void AddSC_TempestKeepBotScripts();

void AddPlayerbotsScripts()
{
    new PlayerbotsBattlefieldScript();
    new PlayerbotsDatabaseScript();
    new PlayerbotsPlayerScript();
    new PlayerbotsMiscScript();
    new PlayerbotsServerScript();
    new PlayerbotsWorldScript();
    new PlayerbotsScript();
    new PlayerBotsBGScript();
    AddPlayerbotsSecureLoginScripts();
    AddPlayerbotsCommandscripts();
    PlayerBotsGuildValidationScript();
    AddSC_TempestKeepBotScripts();
}

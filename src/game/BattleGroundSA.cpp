/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "GameObject.h"
#include "BattleGround.h"
#include "BattleGroundSA.h"
#include "WorldPacket.h"
#include "Language.h"
#include "Player.h"
#include "Object.h"
#include "Creature.h"
#include "BattleGroundMgr.h"
#include "Util.h"
#include "MapManager.h"
#include "ObjectMgr.h"

/*
* BattleGround Strand of the Ancients:
* TODO:
*   - Move all the harcoded variables such coords to header BattleGroundSA.h
*   - Cosmetics & avoid hacks.
*/

static uint32 const BG_SA_GateStatus[6] = {3849, 3623, 3620, 3614, 3617, 3638};
static uint32 const BG_SA_WorldStatusA[3] = {3630, 3627, 3626};
static uint32 const BG_SA_WorldStatusH[3] = {3631, 3628, 3629};

// WorldSafeLocs ids for 5 gyd, and for ally, and horde starting location
static uint32 const BG_SA_GraveyardIdsPhase[3] = {1347, 1346, 1348};
static uint32 const BG_SA_GraveyardIds[2] = {1349, 1350};

static int32 const GrraveYardWS[3][2]=
{
    {3636, 3632},
    {3635, 3633},
    {3637, 3634},
};

static float const BG_SA_START_LOCATIONS[7][4] = {
    {1804.10f, -168.46f, 60.55f, 2.65f},                    // Pillar 1 - don't used now
    {1803.71f, 118.61f, 59.83f, 3.56f},                     // Pillar 2 - don't used now
    {1597.64f, -106.35f, 8.89f, 4.13f},                     // Dock 1
    {1606.61f, 50.13f, 7.58f, 2.39f},                       // Dock 2
    {1209.70f, -65.16f, 70.10f, 0.00f},                     // Defenders start loc
    //Ships
    {2679.696777f, -826.891235f, 3.712860f, 5.78367f},     //rot2 1 rot3 0.0002f
    {2574.003662f, 981.261475f, 2.603424f, 0.807696f}
};

BattleGroundSA::BattleGroundSA()
{
    m_StartMessageIds[BG_STARTING_EVENT_FIRST]  = LANG_BG_SA_START_TWO_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_SECOND] = LANG_BG_SA_START_ONE_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_THIRD]  = LANG_BG_SA_START_HALF_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_FOURTH] = LANG_BG_SA_HAS_BEGUN;

    m_BgObjects.resize(BG_SA_MAXOBJ);
    shipsStarted = false;
    isDemolisherDestroyed[0] = false; // ALLIANCE
    isDemolisherDestroyed[1] = false; // HORDE
    shipsTimer = BG_SA_BOAT_START;
    pillarOpenTimer = BG_SA_PILLAR_START;
    for (int32 i = 0; i <= BG_SA_GATE_MAX; ++i)
        GateStatus[i] = 1;
    TimerEnabled = false;
    TimeST2Round = 120000;
    Round_timer = 0;
    Phase = 1;
}

BattleGroundSA::~BattleGroundSA()
{
}

void BattleGroundSA::FillInitialWorldStates(WorldPacket& data, uint32& count)
{
    if (GetDefender() == HORDE)
    {
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 1);
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 0);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 0);
        
        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN,1);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN,0);
    }
    else
    {
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 0);
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 1);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 1);

        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN,0);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN,1);
    }

    for (uint32 z = 0; z <= BG_SA_GATE_MAX; ++z)
        FillInitialWorldState(data, count, BG_SA_GateStatus[z], GateStatus[z]);

    //Time will be sent on first update...
    FillInitialWorldState(data, count, BG_SA_ENABLE_TIMER, TimerEnabled ? uint32(1) : uint32(0));
    FillInitialWorldState(data, count, BG_SA_TIMER_MINUTES, uint32(0));
    FillInitialWorldState(data, count, BG_SA_TIMER_10SEC, uint32(0));
    FillInitialWorldState(data, count, BG_SA_TIMER_SEC, uint32(0));

    UpdateWorldState(BG_SA_RIGHT_GY_HORDE , ((m_Gyd[0] == BG_SA_GARVE_STATUS_HORDE_CONTESTED) || (m_Gyd[0] == BG_SA_GARVE_STATUS_HORDE_OCCUPIED)) ? 1 : 0);
    UpdateWorldState(BG_SA_LEFT_GY_HORDE , ((m_Gyd[1] == BG_SA_GARVE_STATUS_HORDE_CONTESTED) || (m_Gyd[1] == BG_SA_GARVE_STATUS_HORDE_OCCUPIED)) ? 1 : 0);
    UpdateWorldState(BG_SA_CENTER_GY_HORDE , ((m_Gyd[2] == BG_SA_GARVE_STATUS_HORDE_CONTESTED) || (m_Gyd[2] == BG_SA_GARVE_STATUS_HORDE_OCCUPIED)) ? 1 : 0);

    UpdateWorldState(BG_SA_RIGHT_GY_ALLIANCE , ((m_Gyd[0] == BG_SA_GARVE_STATUS_ALLY_CONTESTED) || (m_Gyd[0] == BG_SA_GARVE_STATUS_ALLY_OCCUPIED)) ? 1 : 0);
    UpdateWorldState(BG_SA_LEFT_GY_ALLIANCE , ((m_Gyd[1] == BG_SA_GARVE_STATUS_ALLY_CONTESTED) || (m_Gyd[1] == BG_SA_GARVE_STATUS_ALLY_OCCUPIED)) ? 1 : 0);
    UpdateWorldState(BG_SA_CENTER_GY_ALLIANCE , ((m_Gyd[2] == BG_SA_GARVE_STATUS_ALLY_CONTESTED) || (m_Gyd[2] == BG_SA_GARVE_STATUS_ALLY_OCCUPIED)) ? 1 : 0);
}

void BattleGroundSA::StartShips()
{
    if (shipsStarted)
        return;

    DoorOpen(m_BgObjects[BG_SA_BOAT_ONE]);
    DoorOpen(m_BgObjects[BG_SA_BOAT_TWO]);

    for (int i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; i++)
    {
        for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* p = sObjectMgr.GetPlayer(itr->first))
            {
                UpdateData data;
                WorldPacket pkt;
                GetBGObject(i)->BuildValuesUpdateBlockForPlayer(&data, p);
                data.BuildPacket(&pkt);
                p->GetSession()->SendPacket(&pkt);
            }
        }
    }
    shipsStarted = true;
}

void BattleGroundSA::ToggleTimer()
{
    TimerEnabled = !TimerEnabled;
    UpdateWorldState(BG_SA_ENABLE_TIMER, (TimerEnabled) ? 1 : 0);
}

void BattleGroundSA::EndBattleGround(Team winner)
{
    //win reward
    if(winner)
    {
        RewardHonorToTeam(GetBonusHonorFromKill(1), winner);
        RewardXpToTeam(0, 0.8f, winner);
    }

    //complete map_end rewards (even if no team wins)
    RewardHonorToTeam(GetBonusHonorFromKill(2), ALLIANCE);
    RewardHonorToTeam(GetBonusHonorFromKill(2), HORDE);
    RewardXpToTeam(0, 0.8f, ALLIANCE);
    RewardXpToTeam(0, 0.8f, HORDE);
    
    BattleGround::EndBattleGround(winner);
}

void BattleGroundSA::Update(uint32 diff)
{
    BattleGround::Update(diff);

    if (GetStatus() == STATUS_WAIT_JOIN)
    {
        if (Phase == SA_ROUND_ONE) // Round one not started yet
        {
            if (!shipsStarted)
            {
                if (shipsTimer <= diff)
                    StartShips();
                else
                    shipsTimer -= diff;
            }

            if (pillarOpenTimer && pillarOpenTimer <= diff)
            {
                SpawnEvent(BG_EVENT_DOOR, 0, false);
                pillarOpenTimer = diff;
            }
            else pillarOpenTimer -= diff;
        }
    }

    if (GetStatus() == STATUS_IN_PROGRESS) // Battleground already in progress
    {
        if (Round_timer >= BG_SA_ROUNDLENGTH)
        {
            if(Phase == SA_ROUND_ONE) // Timeout of first round
            {
                PlaySoundToAll(BG_SA_SOUND_GYD_VICTORY);
                SendMessageToAll(defender == ALLIANCE ? LANG_BG_SA_ALLIANCE_TIMEOUT_END_1ROUND : LANG_BG_SA_HORDE_TIMEOUT_END_1ROUND, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
                SendWarningToAll(LANG_BG_SA_END_1ROUND);
                RoundScores[0].winner = GetDefender();
                RoundScores[0].time = BG_SA_ROUNDLENGTH;

                for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                {
                    if (Player *plr = sObjectMgr.GetPlayer(itr->first))
                        plr->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 52459);
                }

                ResetBattle(0, defender);
            }
            else // Timeout of second round
            {
                SendMessageToAll(defender == ALLIANCE ? LANG_BG_SA_ALLIANCE_TIMEOUT_END_2ROUND : LANG_BG_SA_HORDE_TIMEOUT_END_2ROUND, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
                RoundScores[1].winner = GetDefender();

                for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                {
                    if (Player *plr = sObjectMgr.GetPlayer(itr->first))
                        plr->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 52459);
                }

                if (RoundScores[0].winner == GetDefender())
                    EndBattleGround(GetDefender());
                else
                    EndBattleGround(TEAM_NONE);
                return;
            }
        } 
        else 
            Round_timer += diff;

        for (int gyd = 0; gyd < BG_SA_GRY_MAX; ++gyd)
        {
            // 3 sec delay to spawn new banner instead previous despawned one
            if (m_BannerTimers[gyd].timer)
            {
                if (m_BannerTimers[gyd].timer > diff)
                    m_BannerTimers[gyd].timer -= diff;
                else
                {
                    m_BannerTimers[gyd].timer = 0;
                    _CreateBanner(gyd, m_BannerTimers[gyd].type, m_BannerTimers[gyd].teamIndex, false);
                }
            }
        }
        UpdateTimer();
    }

    if (GetStatus() == STATUS_WAIT_JOIN && Phase == SA_ROUND_TWO) // Round two, not yet started
    {
        if (!shipsStarted)
        {
            if (shipsTimer <= diff)
            {
                SendMessageToAll(LANG_BG_SA_START_ONE_MINUTE, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
                StartShips();
            }
            else
                shipsTimer -= diff;
        }

        if (pillarOpenTimer && pillarOpenTimer <= diff)
        {
            SpawnEvent(BG_EVENT_DOOR, 0, false);
            pillarOpenTimer = diff;
        }
        else pillarOpenTimer -= diff;

        if (TimeST2Round < diff)
        {
            Phase = 2;
            SpawnEvent(SA_EVENT_ADD_NPC, 0, true);
            ToggleTimer();
            SetStatus(STATUS_IN_PROGRESS); // Start round two
            PlaySoundToAll(SOUND_BG_START);
            SendWarningToAll(LANG_BG_SA_HAS_BEGUN);
        }
        else
            TimeST2Round -= diff;
    }
}

void BattleGroundSA::ResetWorldStates()
{
    if (GetDefender() == HORDE)
    {
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 1);
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 0);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 0);

        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN,1);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN,0);
    }
    else
    {
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 1);
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 0);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 1);

        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN,0);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN,1);
    }

    UpdateWorldState(BG_SA_PURPLE_GATEWS, 1);
    UpdateWorldState(BG_SA_RED_GATEWS, 1);
    UpdateWorldState(BG_SA_BLUE_GATEWS, 1);
    UpdateWorldState(BG_SA_GREEN_GATEWS, 1);
    UpdateWorldState(BG_SA_YELLOW_GATEWS, 1);
    UpdateWorldState(BG_SA_ANCIENT_GATEWS, 1);

}

void BattleGroundSA::UpdateTimer()
{
    uint32 end_of_round = (BG_SA_ROUNDLENGTH - Round_timer);
    UpdateWorldState(BG_SA_TIMER_MINUTES, end_of_round/60000);
    UpdateWorldState(BG_SA_TIMER_10SEC, (end_of_round%60000)/10000);
    UpdateWorldState(BG_SA_TIMER_SEC, ((end_of_round%60000)%10000)/1000);
}

void BattleGroundSA::StartingEventCloseDoors()
{
}

void BattleGroundSA::StartingEventOpenDoors()
{
    SpawnEvent(SA_EVENT_ADD_NPC, 0, true);
    ToggleTimer();
    StartTimedAchievement(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, (defender  == HORDE) ? BG_SA_EVENT_START_BATTLE_1 : BG_SA_EVENT_START_BATTLE_2);
}

void BattleGroundSA::RemovePlayer(Player* /*plr*/, ObjectGuid /*guid*/)
{
}

void BattleGroundSA::AddPlayer(Player *plr)
{
    BattleGround::AddPlayer(plr);

    TeleportPlayerToCorrectLoc(plr);

    BattleGroundSAScore* sc = new BattleGroundSAScore;

    m_PlayerScores[plr->GetObjectGuid()] = sc;
}

void BattleGroundSA::HandleAreaTrigger(Player * /*Source*/, uint32 /*Trigger*/)
{
    // this is wrong way to implement these things. On official it done by gameobject spell cast.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;
}

void BattleGroundSA::UpdatePlayerScore(Player* Source, uint32 type, uint32 value)
{
    BattleGroundScoreMap::iterator itr = m_PlayerScores.find(Source->GetObjectGuid());
    if(itr == m_PlayerScores.end())                         // player not found...
        return;

    switch(type)
    {
        case SCORE_DEMOLISHERS_DESTROYED:
            ((BattleGroundSAScore*)itr->second)->DemolishersDestroyed += value;
            break;
        case SCORE_GATES_DESTROYED:
            ((BattleGroundSAScore*)itr->second)->GatesDestroyed += value;
            break;
        default:
            BattleGround::UpdatePlayerScore(Source,type,value);
            break;
    }
}

void BattleGroundSA::ResetBattle(uint32 winner, Team teamDefending)
{
    Phase = SA_ROUND_TWO;
    shipsTimer = 60000;
    pillarOpenTimer = 90000;
    shipsStarted = false;
    
    for (int32 i = 0; i <= BG_SA_GATE_MAX; ++i)
        GateStatus[i] = 1;

    SetStartTime(0);
    defender = (teamDefending  == ALLIANCE) ?  HORDE : ALLIANCE;
    relicGateDestroyed = false;
    ToggleTimer();

    SetupShips();

    for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
    {
        Player *plr = sObjectMgr.GetPlayer(itr->first);

        if (!plr)
            continue;

        TeleportPlayerToCorrectLoc(plr, true);

        if (plr->GetBGTeam() == defender && plr->GetItemByEntry(39213) != NULL)
            plr->DestroyItemCount(39213, 1, true);
    }

    uint32 npcEvent = MAKE_PAIR32(SA_EVENT_ADD_NPC, 0);
    for(std::vector<ObjectGuid>::iterator itr = m_EventObjects[npcEvent].creatures.begin(); itr != m_EventObjects[npcEvent].creatures.end(); ++itr)
    {
        if(Creature * pEventCreature = GetBgMap()->GetCreature((*itr)))
            if(pEventCreature->GetEntry() == 28781)
            {
                float x,y,z,o;
                pEventCreature->DealDamage(pEventCreature, pEventCreature->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                pEventCreature->GetRespawnCoord(x,y,z,&o);
                pEventCreature->NearTeleportTo(x,y,z,o);
            }
    }

    UpdatePhase();
    ResetWorldStates();
}

void BattleGroundSA::Reset()
{
    //call parent's class reset
    BattleGround::Reset();

    defender = ((urand(0,1)) ? ALLIANCE : HORDE);
    relicGateDestroyed = false;

    m_ActiveEvents[SA_EVENT_ADD_GO] = BG_EVENT_NONE;
    m_ActiveEvents[SA_EVENT_ADD_NPC] = BG_EVENT_NONE;
    m_ActiveEvents[SA_EVENT_ADD_SPIR] = BG_EVENT_NONE;
    m_ActiveEvents[SA_EVENT_ADD_BOMB] = BG_EVENT_NONE;
    m_ActiveEvents[SA_EVENT_ADD_VECH_E] = BG_EVENT_NONE;
    m_ActiveEvents[SA_EVENT_ADD_VECH_W] = BG_EVENT_NONE;
    // spiritguides and flags not spawned at beginning
    UpdatePhase();
}

void BattleGroundSA::UpdatePhase()
{
    if (Phase == SA_ROUND_TWO)
    {
        SpawnEvent(SA_EVENT_ADD_VECH_E, 0, false);
        SpawnEvent(SA_EVENT_ADD_VECH_W, 0, false);
        SpawnEvent(SA_EVENT_ADD_BOMB, 0, false);
        SpawnEvent(SA_EVENT_ADD_BOMB, 1, false);
        SpawnEvent(SA_EVENT_ADD_NPC, 0, false);
        SpawnEvent(BG_EVENT_DOOR, 0, true);

        Round_timer = (BG_SA_ROUNDLENGTH - RoundScores[0].time);
        SetStatus(STATUS_WAIT_JOIN);
        SendMessageToAll(LANG_BG_SA_START_TWO_MINUTE, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
    }

    // Spawn bombs at start
    SpawnEvent(SA_EVENT_ADD_BOMB, (GetDefender() == ALLIANCE ? 1 : 0), true);

    _GydOccupied(4, (GetDefender() == HORDE) ? ALLIANCE : HORDE);
    m_ActiveEvents[5] = (GetDefender() == ALLIANCE) ? BG_SA_GARVE_STATUS_ALLY_CONTESTED : BG_SA_GARVE_STATUS_HORDE_CONTESTED;

    // Spawn banners and graveyards
    for (uint8 i = 0; i < BG_SA_GRY_MAX; ++i)
    {
        for (uint8 z = 1; z < 5; ++z)
            SpawnEvent(i, z, false);

        m_BannerTimers[i].timer = 0;
        SpawnEvent(i, (GetDefender() == ALLIANCE ? 1 : 2), true);
        m_Gyd[i] = ((GetDefender() == ALLIANCE) ? BG_SA_GARVE_STATUS_ALLY_CONTESTED : BG_SA_GARVE_STATUS_HORDE_CONTESTED);
        m_ActiveEvents[i] = ((GetDefender() == ALLIANCE) ? BG_SA_GARVE_STATUS_ALLY_CONTESTED : BG_SA_GARVE_STATUS_HORDE_CONTESTED);
        _GydOccupied(i, GetDefender());
    }

    // (Re)spawn graveyard at the beach.
    SpawnEvent(SA_EVENT_ADD_SPIR, BG_SA_GARVE_STATUS_HORDE_CONTESTED, GetDefender() == ALLIANCE ? false : true);
    SpawnEvent(SA_EVENT_ADD_SPIR, BG_SA_GARVE_STATUS_ALLY_CONTESTED, GetDefender() == ALLIANCE ? true : false);

    // Reset world-state of gates
    for (uint32 z = 0; z <= BG_SA_GATE_MAX; ++z)
        UpdateWorldState(BG_SA_GateStatus[z], GateStatus[z]);

    SpawnEvent(SA_EVENT_ADD_GO, 0, false);
    SpawnEvent(SA_EVENT_ADD_GO, 0, true);
}

bool BattleGroundSA::SetupBattleGround()
{
    return SetupShips();
}

bool BattleGroundSA::SetupShips()
{
    for (uint8 i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; ++i)
    {
        for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* plr = sObjectMgr.GetPlayer(itr->first))
                SendTransportsRemove(plr);
        }
    }

    for (uint8 i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; ++i)
    {
        uint32 boatid=0;
        switch (i)
        {
            case BG_SA_BOAT_ONE:
                boatid = GetDefender() == ALLIANCE ? BG_SA_BOAT_ONE_H : BG_SA_BOAT_ONE_A;
                if (!(AddObject(i, boatid, BG_SA_START_LOCATIONS[i + 5][0], BG_SA_START_LOCATIONS[i + 5][1], BG_SA_START_LOCATIONS[i + 5][2]+ (GetDefender() == ALLIANCE ? -3.750f: 0) , BG_SA_START_LOCATIONS[i + 5][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_ONE_DAY)))
                {
                    sLog.outError("SA_ERROR: Can't spawn ships!");
                    return false;
                }

                if (GameObject* boat = GetBGObject(i))
                    boat->SetTransportPathRotation(QuaternionData(0.0f, 0.0f, 1.0f, 0.0002f));
                break;
            case BG_SA_BOAT_TWO:
                boatid = GetDefender() == ALLIANCE ? BG_SA_BOAT_TWO_H : BG_SA_BOAT_TWO_A;
                if (!(AddObject(i, boatid, BG_SA_START_LOCATIONS[i + 5][0], BG_SA_START_LOCATIONS[i + 5][1], BG_SA_START_LOCATIONS[i + 5][2]+ (GetDefender() == ALLIANCE ? -3.750f: 0) , BG_SA_START_LOCATIONS[i + 5][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_ONE_DAY)))
                {
                    sLog.outError("SA_ERROR: Can't spawn ships!");
                    return false;
                }

                if (GameObject* boat = GetBGObject(i))
                    boat->SetTransportPathRotation(QuaternionData(0, 0, 1.0f, 0.00001f));
                break;
        }
    }

    SpawnBGObject(m_BgObjects[BG_SA_BOAT_ONE], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_SA_BOAT_TWO], RESPAWN_IMMEDIATELY);

    for (uint8 i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; ++i)
    {
        for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* plr = sObjectMgr.GetPlayer(itr->first))
                SendTransportInit(plr);
        }
    }
    return true;
}

/*  type: 0-neutral, 1-contested, 3-occupied
    team: 0-ally, 1-horde                        */
void BattleGroundSA::_CreateBanner(uint8 gry, uint8 type, uint8 teamIndex, bool delay)
{
    // Just put it into the queue
    if (delay)
    {
        m_BannerTimers[gry].timer = 2000;
        m_BannerTimers[gry].type = type;
        m_BannerTimers[gry].teamIndex = teamIndex;
        return;
    }

    SpawnEvent(gry, type, true);                           // will automaticly despawn other events
}

void BattleGroundSA::EventPlayerClickedOnFlag(Player *source, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint8 objectEvent = (sBattleGroundMgr.GetGameObjectEventIndex(target_obj->GetGUIDLow())).event1;
    if (objectEvent >= BG_SA_GRY_MAX)                           // not a node
        return;
    BG_SA_GraveYard gyd = BG_SA_GraveYard(objectEvent);

    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(source->GetTeam());

    if (gyd == 2)
        if (GetGateStatus(BG_SA_GO_GATES_T_MAUVE_AMETHYST) != BG_SA_GO_GATES_DESTROY && GetGateStatus(BG_SA_GO_GATES_T_RED_SUN) != BG_SA_GO_GATES_DESTROY)
            return;

    if ((m_Gyd[gyd] == BG_SA_GARVE_STATUS_ALLY_CONTESTED) || (m_Gyd[gyd] == BG_SA_GARVE_STATUS_HORDE_CONTESTED))
    {
        //make the new banner not capturable by defenders
        m_Gyd[gyd] += (GetDefender() == ALLIANCE) ? 3 : 1;
        // create new occupied banner (attacker one, not clickable by anyone)
        _CreateBanner(gyd, (GetDefender() == ALLIANCE ? BG_SA_GARVE_STATUS_HORDE_OCCUPIED : BG_SA_GARVE_STATUS_ALLY_OCCUPIED), teamIndex, true);
        _GydOccupied(gyd,(teamIndex == 0) ? ALLIANCE:HORDE);

        RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
        RewardXpToTeam(0, 0.6f, (teamIndex == 0) ? ALLIANCE:HORDE);

        if (teamIndex == BG_TEAM_ALLIANCE)
        {
            SendWarningToAllSA(gyd, STATUS_CONQUESTED, ALLIANCE);
            PlaySoundToAll(BG_SA_SOUND_GYD_CAPTURED_ALLIANCE);
        }
        else
        {
            SendWarningToAllSA(gyd, STATUS_CONQUESTED, HORDE);
            PlaySoundToAll(BG_SA_SOUND_GYD_CAPTURED_HORDE);
        }
        switch(gyd)
        {
            case 0: SpawnEvent(SA_EVENT_ADD_VECH_E, 0, true);break;
            case 1: SpawnEvent(SA_EVENT_ADD_VECH_W, 0, true);break;
        }
    }
}

void BattleGroundSA::EventSpawnGOSA(Player *owner, Creature* obj, float x, float y, float z)
{
    SendMessageToAll(LANG_BG_SA_INSTALL_BOMB, (defender == ALLIANCE) ? CHAT_MSG_BG_SYSTEM_HORDE : CHAT_MSG_BG_SYSTEM_ALLIANCE , owner);
}

void BattleGroundSA::SendMessageSA(Player *player, uint32 type, uint32 name)
{
    uint32 entryMSG = 0;
    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(player->GetTeam());
    switch (type)
    {
        case 0: entryMSG = LANG_BG_SA_GATE_ATTACK; break;
        case 1: entryMSG = LANG_BG_SA_GATE_DAMAGE; break;
        case 2: entryMSG = LANG_BG_SA_GATE_DETROYED; break;
    }
    if (teamIndex == BG_TEAM_ALLIANCE)
        SendMessage2ToAll(entryMSG,CHAT_MSG_BG_SYSTEM_ALLIANCE, player, name);
    else
        SendMessage2ToAll(entryMSG,CHAT_MSG_BG_SYSTEM_HORDE, player, name);
}

void BattleGroundSA::EventPlayerDamageGO(Player *player, GameObject* target_obj, uint32 eventId, uint32 doneBy)
{
    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(player->GetTeam());

    // Seaforium Charge Explosion
    if (doneBy == 52408)
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 60937);

    uint32 type = NULL;
    switch (target_obj->GetEntry())
    {
        case BG_SA_GO_GATES_ROOM_ANCIENT_SHRINE:
        {
            type = BG_SA_GO_GATES_T_ROOM_ANCIENT_SHRINE;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19836:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19837:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(100, (teamIndex == 0) ? ALLIANCE:HORDE);
                    relicGateDestroyed = true;
                    break;
            }
            break;
        }
        case BG_SA_GO_GATES_GREEN_EMERALD:
        {
            type = BG_SA_GO_GATES_T_GREEN_EMERALD;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19041:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19046:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
                    break;
            }
            break;
        }
        case BG_SA_GO_GATES_BLUE_SAPHIRE:
        {
            type = BG_SA_GO_GATES_T_BLUE_SAPHIRE;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19040:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19045:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
                    break;
            }
            break;
        }
        case BG_SA_GO_GATES_MAUVE_AMETHYST:
        {
            type = BG_SA_GO_GATES_T_MAUVE_AMETHYST;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19043:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19048:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
                    break;
            }
            break;
        }
        case BG_SA_GO_GATES_RED_SUN:
        {
            type = BG_SA_GO_GATES_T_RED_SUN;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19042:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19047:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
                    break;
            }
            break;
        }
        case BG_SA_GO_GATES_YELLOW_MOON:
        {
            type = BG_SA_GO_GATES_T_YELLOW_MOON;
            switch (eventId)
            {
                case 21630:
                    break;
                case 19044:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DAMAGE);
                    break;
                case 19049:
                    SendWarningToAllSA(NULL, NULL, TEAM_NONE, true, type, true);
                    UpdateWorldState(BG_SA_GateStatus[type], GateStatus[type] = BG_SA_GO_GATES_DESTROY);
                    UpdatePlayerScore(player, SCORE_GATES_DESTROYED, 1);
                    RewardHonorToTeam(85, (teamIndex == 0) ? ALLIANCE:HORDE);
                    break;
            }
            break;
        }
        case BG_SA_GO_TITAN_RELIC:
        {
            if (eventId == 22097 && player->GetTeam() != GetDefender())
            {
                if(!relicGateDestroyed)
                {
                    player->GetSession()->KickPlayer();
                    sLog.outError("Player %s has clicked SOTA Relic without Relic gate being destroyed", player->GetName());
                    return;
                }

                //Achievement Storm the Beach (1310)
                for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                {
                    if (Player *plr = sObjectMgr.GetPlayer(itr->first))
                        if (plr->GetTeam() != defender)
                            plr->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 65246);
                }

                if(Phase == SA_ROUND_ONE) // Victory at first round
                {

                    RoundScores[0].winner = GetDefender() == ALLIANCE ? HORDE : ALLIANCE;
                    RoundScores[0].time = Round_timer;
                    PlaySoundToAll(BG_SA_SOUND_GYD_VICTORY);
                    SendMessageToAll(defender == HORDE ? LANG_BG_SA_ALLIANCE_END_1ROUND : LANG_BG_SA_HORDE_END_1ROUND, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
                    SendWarningToAll(LANG_BG_SA_END_1ROUND);
                    RewardHonorToTeam(150, (teamIndex == 0) ? ALLIANCE:HORDE);
                    ResetBattle(player->GetTeam(), GetDefender());
                }
                else // Victory at second round
                {
                    RoundScores[1].winner = GetDefender() == ALLIANCE ? HORDE : ALLIANCE;
                    SendMessageToAll(defender == HORDE ? LANG_BG_SA_ALLIANCE_END_2ROUND : LANG_BG_SA_HORDE_END_2ROUND, CHAT_MSG_BG_SYSTEM_NEUTRAL, NULL);
                    RewardHonorToTeam(150, (teamIndex == 0) ? ALLIANCE:HORDE);
                    EndBattleGround(player->GetTeam());
                }
            }
            break;
        }
    }
}

void BattleGroundSA::HandleKillPlayer(Player* player, Player* killer)
{

    player->CastSpell(player, 52417, false);

    BattleGround::HandleKillPlayer(player, killer);
}

void BattleGroundSA::HandleKillUnit(Creature* unit, Player* killer)
{
    if(!unit)
        return;

    if(unit->GetEntry() == 28781)  //Demolisher
    {
        UpdatePlayerScore(killer, SCORE_DEMOLISHERS_DESTROYED, 1);
        isDemolisherDestroyed[killer->GetTeam() == HORDE ? 0 : 1] = true;
    }
}

int32 BattleGroundSA::_GatesName(GameObject* obj)
{
    if (!obj)
        return 0;
    switch (obj->GetEntry())
    {
        case BG_SA_GO_GATES_ROOM_ANCIENT_SHRINE:  return LANG_BG_SA_GATE_ROOM_ANCIENT_SHRINE;
        case BG_SA_GO_GATES_GREEN_EMERALD:  return LANG_BG_SA_GATE_GREEN_EMERALD;
        case BG_SA_GO_GATES_BLUE_SAPHIRE:  return LANG_BG_SA_GATE_BLUE_SAPHIRE;
        case BG_SA_GO_GATES_MAUVE_AMETHYST:  return LANG_BG_SA_GATE_MAUVE_AMETHYST;
        case BG_SA_GO_GATES_RED_SUN:  return LANG_BG_SA_GATE_RED_SUN_;
        case BG_SA_GO_GATES_YELLOW_MOON:  return LANG_BG_SA_GATE_YELLOW_MOON;
        default:
            MANGOS_ASSERT(0);
    }
    return 0;
}

int32 BattleGroundSA::_GydName(uint8 gyd)
{
    switch (gyd)
    {
        case 0:  return LANG_BG_SA_EAST_GRAVEYARD;
        case 1:  return LANG_BG_SA_WEST_GRAVEYARD;
        case 2:  return LANG_BG_SA_SOUTH_GRAVEYARD;
        default:
            MANGOS_ASSERT(0);
    }
    return 0;
}

WorldSafeLocsEntry const* BattleGroundSA::GetClosestGraveYard(Player* player)
{
    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(player->GetTeam());

    // Is there any occupied node for this team?
    std::vector<uint8> gyd;
    for (uint8 i = 0; i < BG_SA_GRY_MAX; ++i)
        // players should be able to ressurect at their faction's contested/occupied graveyards too
        if ((m_Gyd[i] == teamIndex + 1) || (m_Gyd[i] == teamIndex + 3)) 
            gyd.push_back(i);

    WorldSafeLocsEntry const* good_entry = NULL;
    // If so, select the closest node to place ghost on
    if (!gyd.empty())
    {
        float plr_x = player->GetPositionX();
        float plr_y = player->GetPositionY();

        float mindist = 999999.0f;
        for (uint8 i = 0; i < gyd.size(); ++i)
        {
            WorldSafeLocsEntry const*entry = sWorldSafeLocsStore.LookupEntry(BG_SA_GraveyardIdsPhase[gyd[i]]);
            if (!entry)
                continue;
            float dist = (entry->x - plr_x)*(entry->x - plr_x)+(entry->y - plr_y)*(entry->y - plr_y);
            if (mindist > dist)
            {
                mindist = dist;
                good_entry = entry;
            }
        }
        gyd.clear();
    }
    // If not, place ghost on starting location
    if (!good_entry)
    {
        if (GetDefender() == HORDE)
        {
            if (teamIndex == 0)
                good_entry = sWorldSafeLocsStore.LookupEntry(BG_SA_GraveyardIds[1]);
            else
                good_entry = sWorldSafeLocsStore.LookupEntry(BG_SA_GraveyardIds[0]);
        }
        if (GetDefender() == ALLIANCE)
        {
            if (teamIndex == 0)
                good_entry = sWorldSafeLocsStore.LookupEntry(BG_SA_GraveyardIds[0]);
            else
                good_entry = sWorldSafeLocsStore.LookupEntry(BG_SA_GraveyardIds[1]);
        }
    }
    return good_entry;
}

void BattleGroundSA::_GydOccupied(uint8 node, Team team)
{
    if (node >= 0 && node < 4)
    {
        UpdateWorldState(GrraveYardWS[node][0], team == HORDE ? 0 : 1);
        UpdateWorldState(GrraveYardWS[node][1], team == HORDE ? 1 : 0);
    }
    else if (node == 4)
    {
        for (int8 i = 0; i <= BG_SA_MAX_WS; ++i)
        {
            UpdateWorldState(BG_SA_WorldStatusH[i], team == HORDE ? 1 : 0);
            UpdateWorldState(BG_SA_WorldStatusA[i], team == HORDE ? 0 : 1);
        }
    }
}

void BattleGroundSA::SendWarningToAllSA(uint8 gyd, int status, Team team, bool isDoor, int door, bool destroyed)
{
    if (!isDoor)
    {
        if (status == STATUS_CONQUESTED)
        {
            if (team == HORDE)
            {
                switch(gyd)
                {
                    case 0: SendWarningToAll(LANG_BG_SA_HORDE_EAST_CONQUESTED); break;
                    case 1: SendWarningToAll(LANG_BG_SA_HORDE_WEST_CONQUESTED); break;
                    case 2: SendWarningToAll(LANG_BG_SA_HORDE_SOUTH_CONQUESTED); break;
                    default: sLog.outError("Error in SA strings: Unknow graveyard %u", gyd); break;
                }
            }
            else
            {
                switch(gyd)
                {
                    case 0: SendWarningToAll(LANG_BG_SA_ALLIANCE_EAST_CONQUESTED); break;
                    case 1: SendWarningToAll(LANG_BG_SA_ALLIANCE_WEST_CONQUESTED); break;
                    case 2: SendWarningToAll(LANG_BG_SA_ALLIANCE_SOUTH_CONQUESTED); break;
                    default: sLog.outError("Error in SA strings: Unknow graveyard %u", gyd); break;
                }
            }
        }
    }
    else
    {
        if(destroyed)
        {
            switch(door)
            {
                case BG_SA_GO_GATES_T_ROOM_ANCIENT_SHRINE: SendWarningToAll(LANG_BG_SA_GATE_ROOM_ANCIENT_SHRINE_DESTROYED); break;
                case BG_SA_GO_GATES_T_GREEN_EMERALD: SendWarningToAll(LANG_BG_SA_GATE_GREEN_EMERALD_DESTROYED); break;
                case BG_SA_GO_GATES_T_BLUE_SAPHIRE: SendWarningToAll(LANG_BG_SA_GATE_BLUE_SAPHIRE_DESTROYED); break;
                case BG_SA_GO_GATES_T_MAUVE_AMETHYST: SendWarningToAll(LANG_BG_SA_GATE_MAUVE_AMETHYST_DESTROYED); break;
                case BG_SA_GO_GATES_T_RED_SUN: SendWarningToAll(LANG_BG_SA_GATE_RED_SUN_DESTROYED); break;
                case BG_SA_GO_GATES_T_YELLOW_MOON: SendWarningToAll(LANG_BG_SA_GATE_YELLOW_MOON_DESTROYED); break;
                default:
                    sLog.outError("Error in SA strings: Unknow door %d", door); break;
            }
        }
        else
        {
            switch(door)
            {
                case BG_SA_GO_GATES_T_ROOM_ANCIENT_SHRINE: SendWarningToAll(LANG_BG_SA_GATE_ROOM_ANCIENT_SHRINE_DAMAGED); break;
                case BG_SA_GO_GATES_T_GREEN_EMERALD: SendWarningToAll(LANG_BG_SA_GATE_GREEN_EMERALD_DAMAGED); break;
                case BG_SA_GO_GATES_T_BLUE_SAPHIRE: SendWarningToAll(LANG_BG_SA_GATE_BLUE_SAPHIRE_DAMAGED); break;
                case BG_SA_GO_GATES_T_MAUVE_AMETHYST: SendWarningToAll(LANG_BG_SA_GATE_MAUVE_AMETHYST_DAMAGED); break;
                case BG_SA_GO_GATES_T_RED_SUN: SendWarningToAll(LANG_BG_SA_GATE_RED_SUN_DAMAGED); break;
                case BG_SA_GO_GATES_T_YELLOW_MOON: SendWarningToAll(LANG_BG_SA_GATE_YELLOW_MOON_DAMAGED); break;
                default:
                    sLog.outError("Error in SA strings: Unknow door %d", door); break;
            }
        }
    }
}

void BattleGroundSA::TeleportPlayerToCorrectLoc(Player *plr, bool resetBattle)
{
    if (!plr)
        return;

    if (plr->GetTeam() == GetDefender())
        plr->TeleportTo(607, 1209.7f, -65.16f, 70.1f, 0.0f, 0); // Defenders position
    else
    {
        if (!shipsStarted)
        {
            if (urand(0,1))
                plr->TeleportTo(607, 2682.936f, -830.368f, 15.0f, 2.895f, 0); // Ship Right
            else
                plr->TeleportTo(607, 2577.003f, 980.261f, 15.0f, 0.807f, 0); // Ship Left
        }
        else if (GetStartTime() < (105 * IN_MILLISECONDS))
        {
            if (urand(0,1))
                plr->TeleportTo(607, 1804.10f, -168.46f, 60.55f, 2.65f, 0); // Pillar Left
            else
                plr->TeleportTo(607, 1803.71f, 118.61f, 59.83f, 3.56f, 0); // Pillar Right
        }
        else // If BG starts in 30 sec it teleports directly at the beach
        {
            if (urand(0,1))
                plr->TeleportTo(607, 1597.64f, -106.35f, 8.89f, 4.13f, 0);
            else
                plr->TeleportTo(607, 1606.61f, 50.13f, 7.58f, 2.39f, 0);
        }
    }
    SendTransportInit(plr);
    if (resetBattle)
    {
        if (!plr->isAlive())
        {
            plr->ResurrectPlayer(1.0f);
            plr->SpawnCorpseBones();
        }

        plr->RemoveArenaAuras(true);
        plr->SetHealth(plr->GetMaxHealth());
        plr->SetPower(POWER_MANA, plr->GetMaxPower(POWER_MANA));
        plr->CombatStopWithPets(true);
    }
}

void BattleGroundSA::SendTransportInit(Player *player)
{
    if (GetBGObject(BG_SA_BOAT_ONE) || GetBGObject(BG_SA_BOAT_TWO))
    {
        UpdateData transData;
        if (GetBGObject(BG_SA_BOAT_ONE))
            GetBGObject(BG_SA_BOAT_ONE)->BuildCreateUpdateBlockForPlayer(&transData, player);
        if (GetBGObject(BG_SA_BOAT_TWO))
            GetBGObject(BG_SA_BOAT_TWO)->BuildCreateUpdateBlockForPlayer(&transData, player);
        WorldPacket packet;
        transData.BuildPacket(&packet);
        player->GetSession()->SendPacket(&packet);
    }
}

void BattleGroundSA::SendTransportsRemove(Player * player)
{
    if (GetBGObject(BG_SA_BOAT_ONE) || GetBGObject(BG_SA_BOAT_TWO))
    {
        UpdateData transData;
        if (GameObject * boat1 = GetBGObject(BG_SA_BOAT_ONE))
        {
            boat1->BuildOutOfRangeUpdateBlock(&transData);
            boat1->SetRespawnTime(0);
            boat1->Delete();
        }
        if (GameObject * boat2 = GetBGObject(BG_SA_BOAT_TWO))
        {
            boat2->BuildOutOfRangeUpdateBlock(&transData);
            boat2->SetRespawnTime(0);
            boat2->Delete();
        }
        WorldPacket packet;
        transData.BuildPacket(&packet);
        player->GetSession()->SendPacket(&packet);
    }
}

uint32 BattleGroundSA::GetCorrectFactionSA(uint8 vehicleType) const
{
    if (GetStatus() != STATUS_WAIT_JOIN)
    {
        switch(vehicleType)
        {
            case VEHICLE_SA_DEMOLISHER:
            {
                if (GetDefender() == ALLIANCE)
                    return VEHICLE_FACTION_HORDE;
                else
                    return VEHICLE_FACTION_ALLIANCE;
            }
            case VEHICLE_SA_CANNON:
            {
                if (GetDefender() == ALLIANCE)
                    return VEHICLE_FACTION_ALLIANCE;
                else
                    return VEHICLE_FACTION_HORDE;
            }
            default:
                return VEHICLE_FACTION_NEUTRAL;
        }
    }
    return VEHICLE_FACTION_NEUTRAL;
}

bool BattleGroundSA::winSAwithAllWalls(Team team)
{
   if (GetDefender() != team)
        return false;

    bool allNotDestroyed = true;

    for (uint32 i = 0; i < BG_SA_GATE_MAX; ++i)
        if (GateStatus[i] == BG_SA_GO_GATES_DESTROY)
            allNotDestroyed = false;

    return allNotDestroyed;
}

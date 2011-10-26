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

#include "Player.h"
#include "BattleGround.h"
#include "BattleGroundIC.h"
#include "BattleGroundMgr.h"
#include "Language.h"
#include "WorldPacket.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "Vehicle.h"
#include "Creature.h"
#include "MapManager.h"
#include "Transports.h"
#include "Object.h"
#include "Util.h"

static uint32 const BG_IC_GateStatus[8] = {4327, 4326, 4328, 4317, 4318, 4319, 4339, 4345};

BattleGroundIC::BattleGroundIC()
{
    m_StartMessageIds[BG_STARTING_EVENT_FIRST]  = LANG_BG_IC_START_TWO_MINUTES;
    m_StartMessageIds[BG_STARTING_EVENT_SECOND] = LANG_BG_IC_START_ONE_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_THIRD]  = LANG_BG_IC_START_HALF_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_FOURTH] = LANG_BG_IC_HAS_BEGUN;

    m_BgObjects.resize(BG_IC_MAXOBJ);
}

BattleGroundIC::~BattleGroundIC()
{
}

void BattleGroundIC::Reset()
{
    //call parent's class reset
    BattleGround::Reset();

    for (uint8 i = 0; i < BG_IC_GATE_MAX; i++)
        GateStatus[i] = BG_IC_GO_GATES_NORMAL;

    closeFortressDoorsTimer = CLOSE_DOORS_TIME;
    doorsClosed = false;

    for (uint8 i = 0; i < BG_TEAMS_COUNT; ++i)
    {
        m_TeamScores[i] = BG_IC_SCORE_INITIAL_POINTS;
    }

    for (uint8 i = 0; i < BG_IC_NODES_MAX; ++i)
    {
        m_prevNodes[i] = 0;
        m_NodeTimers[i] = 0;
        m_BannerTimers[i].timer = 0;
        
        // all nodes except keeps are owned by neutral team at beginning
        if (i < BG_IC_NODE_A_KEEP)
        {
            m_ActiveEvents[i] = BG_IC_NODE_TYPE_NEUTRAL;
            m_Nodes[i] = 0;
        }
    }

    for (uint8 node = BG_IC_NODE_QUARRY; node <= BG_IC_NODE_REFINERY; node++)
    {
        m_resource_Timer[node] = BG_IC_RESOURCE_TICK_TIMER;
    }

    m_ActiveEvents[BG_IC_NODE_A_KEEP] = BG_IC_NODE_STATUS_ALLY_OCCUPIED;
    m_ActiveEvents[BG_IC_NODE_H_KEEP] = BG_IC_NODE_STATUS_HORDE_OCCUPIED;
    m_Nodes[BG_IC_NODE_A_KEEP] = BG_IC_NODE_STATUS_ALLY_OCCUPIED;
    m_Nodes[BG_IC_NODE_H_KEEP] = BG_IC_NODE_STATUS_HORDE_OCCUPIED;

    SpawnEvent(IC_EVENT_ADD_TELEPORT, 0, true);
    SpawnEvent(IC_EVENT_ADD_NPC, 0, false);
    SpawnEvent(IC_EVENT_ADD_A_BOSS_GATE, 0, true);
    SpawnEvent(IC_EVENT_ADD_H_BOSS_GATE, 0, true);
    SpawnEvent(IC_EVENT_ADD_A_BOSS, 0, false);
    SpawnEvent(IC_EVENT_ADD_H_BOSS, 0, false);

    SpawnGates();

    gunshipHorde = NULL;
    gunshipAlliance = NULL;
}

void BattleGroundIC::SendTransportInit(Player* player)
{
    if (!gunshipAlliance || !gunshipHorde)
        return;

    UpdateData transData;

    gunshipAlliance->BuildCreateUpdateBlockForPlayer(&transData, player);
    gunshipHorde->BuildCreateUpdateBlockForPlayer(&transData, player);

    WorldPacket packet;

    transData.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

/*void BattleGroundIC::DoAction(uint32 action, Player *plr)
{
    if (action != ACTION_TELEPORT_PLAYER_TO_TRANSPORT)
        return;

    if (!plr || !gunshipAlliance || !gunshipHorde)
        return;

    float x, y, z;
    if (plr->GetTeam() == TEAM_ALLIANCE)
    {
        gunshipAlliance->GetPosition(x, y, z);
        plr->TeleportTo(GetMapId(), x, y, (z + 25.0f), (plr->GetTeam() == TEAM_ALLIANCE ? gunshipAlliance : gunshipHorde)->GetOrientation(), TELE_TO_NOT_LEAVE_TRANSPORT);
    }
    else
    {
        // This is not the correct way to do this, but it seems that horde gunship has a problem with waypoints that cause unexpected behaviour when following the path.
        plr->TeleportTo(GetMapId(), 660.453003f, -1236.030151f, 288.643402f, 1.6f);
    }
}*/

void BattleGroundIC::UpdateScore(BattleGroundTeamIndex teamIdx, int32 points )
{
    // note: to remove reinforcements points must be negative, for adding reinforcements points must be positive
    MANGOS_ASSERT( teamIdx == BG_TEAM_ALLIANCE || teamIdx == BG_TEAM_HORDE);
    m_TeamScores[teamIdx] += points;                      // m_TeamScores is int32 - so no problems here

    if (points < 0)
    {
        if (m_TeamScores[teamIdx] < 1)
        {
            m_TeamScores[teamIdx] = 0;
            // other team will win:
            EndBattleGround((teamIdx == BG_TEAM_ALLIANCE)? HORDE : ALLIANCE);
        }
    }
    // must be called here, else it could display a negative value
    UpdateWorldState(((teamIdx == BG_TEAM_HORDE) ? BG_TEAM_HORDE_REINFORC : BG_TEAM_ALLIANCE_REINFORC), m_TeamScores[teamIdx]);
}

void BattleGroundIC::Update(uint32 diff)
{
    BattleGround::Update(diff);

    if (GetStatus() == STATUS_IN_PROGRESS)

        if (!doorsClosed)
        {
            if (closeFortressDoorsTimer <= diff)
            {
                GetBGObject(BG_IC_GO_ALLIANCE_GATE_3)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);
                GetBGObject(BG_IC_GO_HORDE_GATE_3)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);

                GetBGObject(BG_IC_GO_A_FRONT)->RemoveFromWorld();
                GetBGObject(BG_IC_GO_H_FRONT)->RemoveFromWorld();

                doorsClosed = true;
            } else closeFortressDoorsTimer -= diff;
        }

        for (int node = 0; node < BG_IC_NODES_MAX; ++node)
        {
            // 3 sec delay to spawn new banner instead previous despawned one
            if (m_BannerTimers[node].timer)
            {
                if (m_BannerTimers[node].timer > diff)
                    m_BannerTimers[node].timer -= diff;
                else
                {
                    m_BannerTimers[node].timer = 0;
                    _CreateBanner(node, m_BannerTimers[node].type, m_BannerTimers[node].teamIndex, false);
                }
            }

            // 1-minute to occupy a node from contested state
            if (m_NodeTimers[node])
            {
                if (m_NodeTimers[node] > diff)
                    m_NodeTimers[node] -= diff;
                else
                {
                    m_NodeTimers[node] = 0;
                    // Change from contested to occupied !
                    uint8 teamIndex = m_Nodes[node]-1;
                    m_prevNodes[node] = m_Nodes[node];
                    m_Nodes[node] += 2;
                    // create new occupied banner
                    _CreateBanner(node, BG_IC_NODE_TYPE_OCCUPIED, teamIndex, true);
                    _SendNodeUpdate(node);
                    // Message to chatlog
                    if (teamIndex == BG_TEAM_ALLIANCE)
                    {
                        SendMessage2ToAll(LANG_BG_IC_TEAM_HAS_TAKEN_NODE,CHAT_MSG_BG_SYSTEM_ALLIANCE,NULL,LANG_BG_ALLY,_GetNodeNameId(node));
                        PlaySoundToAll(BG_IC_SOUND_NODE_CAPTURED_ALLIANCE);
                    }
                    else
                    {
                        SendMessage2ToAll(LANG_BG_IC_TEAM_HAS_TAKEN_NODE,CHAT_MSG_BG_SYSTEM_HORDE,NULL,LANG_BG_HORDE,_GetNodeNameId(node));
                        PlaySoundToAll(BG_IC_SOUND_NODE_CAPTURED_HORDE);
                    }

                    // starting the gunship
                    if (node == BG_IC_NODE_HANGAR)
                        (teamIndex == BG_TEAM_ALLIANCE ? gunshipAlliance : gunshipHorde)->BuildStartMovePacket(GetBgMap());
                }
            }
        }

    // add a point every 45 secs to quarry/refinery owner
    for (uint8 node = BG_IC_NODE_QUARRY; node <= BG_IC_NODE_REFINERY; node++)
    {
        if (m_Nodes[node] >= BG_IC_NODE_TYPE_OCCUPIED)
        {
            if (m_resource_Timer[node] <= diff)
            {
                UpdateScore(BattleGroundTeamIndex(m_Nodes[node] - BG_IC_NODE_TYPE_OCCUPIED) , 1);
                RewardHonorToTeam(GetBonusHonorFromKill(1), (m_Nodes[node] - BG_IC_NODE_TYPE_OCCUPIED == 0 ? ALLIANCE : HORDE));
                m_resource_Timer[node] = BG_IC_RESOURCE_TICK_TIMER;
            } 
            else 
                m_resource_Timer[node] -= diff;
        } 
    }

    HandleBuffs();
}

void BattleGroundIC::StartingEventCloseDoors()
{
    // Show Full Gate Displays
    GetBGObject(BG_IC_GO_ALLIANCE_GATE_1)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);
    GetBGObject(BG_IC_GO_ALLIANCE_GATE_2)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);
    GetBGObject(BG_IC_GO_HORDE_GATE_1)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);
    GetBGObject(BG_IC_GO_HORDE_GATE_2)->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_DAMAGED);
}

void BattleGroundIC::StartingEventOpenDoors()
{
    OpenDoorEvent(BG_EVENT_DOOR);                // used for activating teleport effects + opening tower gates
    DoorOpen(m_BgObjects[BG_IC_GO_A_FRONT]);
    DoorOpen(m_BgObjects[BG_IC_GO_H_FRONT]);
    SpawnEvent(IC_EVENT_ADD_NPC, 0, true);

    uint32 objEvent = MAKE_PAIR32(IC_EVENT_ADD_TELEPORT, 0);     // make teleporters clickable
    for (std::vector<ObjectGuid>::iterator itr = m_EventObjects[objEvent].gameobjects.begin(); itr != m_EventObjects[objEvent].gameobjects.end(); ++itr)
        if (GameObject * pEventGameObject = GetBgMap()->GetGameObject((*itr)))
            if ((pEventGameObject->GetEntry() == 195313) || (pEventGameObject->GetEntry() == 195314) || (pEventGameObject->GetEntry() == 195315) || (pEventGameObject->GetEntry() == 195316))
                if (pEventGameObject->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT))
                    pEventGameObject->RemoveFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT);
}

void BattleGroundIC::AddPlayer(Player *plr)
{
    BattleGround::AddPlayer(plr);
    //create score and add it to map, default values are set in constructor
    BattleGroundICScore* sc = new BattleGroundICScore;

    m_PlayerScores[plr->GetObjectGuid()] = sc;

    SendTransportInit(plr);

    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        uint32 objEvent = MAKE_PAIR32(IC_EVENT_ADD_TELEPORT, 0);
        for (std::vector<ObjectGuid>::iterator itr = m_EventObjects[objEvent].gameobjects.begin(); itr != m_EventObjects[objEvent].gameobjects.end(); ++itr)
            if (GameObject * pEventGameObject = GetBgMap()->GetGameObject((*itr)))
                if ((pEventGameObject->GetEntry() == 195313) || (pEventGameObject->GetEntry() == 195314) || (pEventGameObject->GetEntry() == 195315) || (pEventGameObject->GetEntry() == 195316))
                    if (pEventGameObject->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT))
                        pEventGameObject->RemoveFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT);
    }
}

void BattleGroundIC::RemovePlayer(Player* plr, uint64 /*guid*/)
{
    if (plr)
    {
        plr->RemoveAurasDueToSpell(SPELL_QUARRY);
        plr->RemoveAurasDueToSpell(SPELL_REFINERY);
    }
}

void BattleGroundIC::HandleAreaTrigger(Player * /*Source*/, uint32 /*Trigger*/)
{
    // this is wrong way to implement these things. On official it done by gameobject spell cast.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;
}

/*  type: 0-neutral, 1-contested, 3-occupied
    teamIndex: 0-ally, 1-horde                        */
void BattleGroundIC::_CreateBanner(uint8 node, uint8 type, uint8 teamIndex, bool delay)
{
    // Just put it into the queue
    if (delay)
    {
        m_BannerTimers[node].timer = 2000;
        m_BannerTimers[node].type = type;
        m_BannerTimers[node].teamIndex = teamIndex;
        return;
    }

    // cause the node-type is in the generic form
    // please see in the headerfile for the ids
    if (type != BG_IC_NODE_TYPE_NEUTRAL)
        type += teamIndex;

    SpawnEvent(node, type, true);                           // will automaticly despawn other events
}

int32 BattleGroundIC::_GetNodeNameId(uint8 node)
{
    switch (node)
    {
        case BG_IC_NODE_DOCKS:    return LANG_BG_IC_NODE_DOCKS;
        case BG_IC_NODE_HANGAR:   return LANG_BG_IC_NODE_HANGAR;
        case BG_IC_NODE_WORKSHOP: return LANG_BG_IC_NODE_WORKSHOP;
        case BG_IC_NODE_QUARRY:   return LANG_BG_IC_NODE_QUARRY;
        case BG_IC_NODE_REFINERY: return LANG_BG_IC_NODE_REFINERY;
        case BG_IC_NODE_A_KEEP:   return LANG_BG_IC_ALLIANCE_KEEP;
        case BG_IC_NODE_H_KEEP:   return LANG_BG_IC_HORDE_KEEP;
        default:
            MANGOS_ASSERT(0);
    }
    return 0;
}

void BattleGroundIC::UpdatePlayerScore(Player* Source, uint32 type, uint32 value, bool doAddHonor)
{
    BattleGroundScoreMap::iterator itr = m_PlayerScores.find(Source->GetObjectGuid());

    if (itr == m_PlayerScores.end())                         // player not found...
        return;

    switch(type)
    {
        case SCORE_BASES_ASSAULTED:
            ((BattleGroundICScore*)itr->second)->BasesAssaulted += value;
            break;
        case SCORE_BASES_DEFENDED:
            ((BattleGroundICScore*)itr->second)->BasesDefended += value;
            break;
        default:
            BattleGround::UpdatePlayerScore(Source, type, value);
            break;
    }
}

void BattleGroundIC::FillInitialWorldStates(WorldPacket& data, uint32& count)
{
    FillInitialWorldState(data, count, BG_TEAM_ALLIANCE_REINFORC_SET, BG_TEAM_ALLIANCE_REINFORC_SET);
    FillInitialWorldState(data, count, BG_TEAM_HORDE_RENFORC_SET,     BG_TEAM_HORDE_RENFORC_SET);
    FillInitialWorldState(data, count, BG_TEAM_ALLIANCE_REINFORC,     m_TeamScores[BG_TEAM_ALLIANCE]);
    FillInitialWorldState(data, count, BG_TEAM_HORDE_REINFORC,        m_TeamScores[BG_TEAM_HORDE]);

    for (uint8 z = 0; z <= BG_IC_GATE_MAX; ++z)
        FillInitialWorldState(data, count, BG_IC_GateStatus[z], GateStatus[z]);

    // Node icons
    for (uint8 node = 0; node < BG_IC_NODES_MAX; ++node)
        FillInitialWorldState(data, count, BG_IC_OP_NODEICONS[node], m_Nodes[node] == 0);

    for (uint8 i = 0; i < BG_IC_NODES_MAX; ++i)
        for (uint8 j = 0; j < 4; j++)
            FillInitialWorldState(data, count, BG_IC_NodeWorldStates[i][j], m_Nodes[i] == (j + 1));
}

bool BattleGroundIC::SetupBattleGround()
{
    gunshipHorde = CreateTransport(GO_HORDE_GUNSHIP,TRANSPORT_PERIOD_TIME);
    gunshipAlliance = CreateTransport(GO_ALLIANCE_GUNSHIP,TRANSPORT_PERIOD_TIME);

    if (!gunshipAlliance || !gunshipHorde)
    {
        sLog.outError("Isle of Conquest: There was an error creating gunships!");
        return false;
    }

    //Send transport init packet to all player in map
    for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end();itr++)
    {
        if (Player* player = sObjectMgr.GetPlayer(itr->first))
            SendTransportInit(player);
    }

    return true;
}

void BattleGroundIC::SpawnGates()
{
    AddObject(BG_IC_GO_ALLIANCE_GATE_1, GO_ALLIANCE_GATE_1, BG_IC_GATELOCS[0][0], BG_IC_GATELOCS[0][1], BG_IC_GATELOCS[0][2], BG_IC_GATELOCS[0][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_ALLIANCE_GATE_2, GO_ALLIANCE_GATE_2, BG_IC_GATELOCS[1][0], BG_IC_GATELOCS[1][1], BG_IC_GATELOCS[1][2], BG_IC_GATELOCS[1][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_ALLIANCE_GATE_3, GO_ALLIANCE_GATE_3, BG_IC_GATELOCS[2][0], BG_IC_GATELOCS[2][1], BG_IC_GATELOCS[2][2], BG_IC_GATELOCS[2][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_HORDE_GATE_1, GO_HORDE_GATE_1, BG_IC_GATELOCS[3][0], BG_IC_GATELOCS[3][1], BG_IC_GATELOCS[3][2], BG_IC_GATELOCS[3][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_HORDE_GATE_2, GO_HORDE_GATE_2, BG_IC_GATELOCS[4][0], BG_IC_GATELOCS[4][1], BG_IC_GATELOCS[4][2], BG_IC_GATELOCS[4][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_HORDE_GATE_3, GO_HORDE_GATE_3, BG_IC_GATELOCS[5][0], BG_IC_GATELOCS[5][1], BG_IC_GATELOCS[5][2], BG_IC_GATELOCS[5][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_A_FRONT, GO_ALLIANCE_FRONT, BG_IC_GATELOCS[6][0], BG_IC_GATELOCS[6][1], BG_IC_GATELOCS[6][2], BG_IC_GATELOCS[6][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);
    AddObject(BG_IC_GO_H_FRONT, GO_HORDE_FRONT, BG_IC_GATELOCS[7][0], BG_IC_GATELOCS[7][1], BG_IC_GATELOCS[7][2], BG_IC_GATELOCS[7][3], 0.0f, 0.0f, 0.0f, 0.0f, RESPAWN_IMMEDIATELY);

    SpawnBGObject(m_BgObjects[BG_IC_GO_ALLIANCE_GATE_1], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_ALLIANCE_GATE_2], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_ALLIANCE_GATE_3], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_HORDE_GATE_1], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_HORDE_GATE_2], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_HORDE_GATE_3], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_A_FRONT], RESPAWN_IMMEDIATELY);
    SpawnBGObject(m_BgObjects[BG_IC_GO_H_FRONT], RESPAWN_IMMEDIATELY);
}

void BattleGroundIC::HandleKillUnit(Creature *creature, Player *killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
       return;
}

void BattleGroundIC::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattleGround::HandleKillPlayer(player, killer);
    UpdateScore(GetTeamIndexByTeamId(player->GetTeam()), -1);
}

void BattleGroundIC::EndBattleGround(Team winner)
{
    //win reward
    if (winner)
    {
        RewardHonorToTeam(GetBonusHonorFromKill(1), winner);
        RewardXpToTeam(0, 0.8f, winner);
    }

    //complete map_end rewards (even if no team wins)
    RewardHonorToTeam(GetBonusHonorFromKill(2), HORDE);
    RewardHonorToTeam(GetBonusHonorFromKill(2), ALLIANCE);
    RewardXpToTeam(0, 0.8f, ALLIANCE);
    RewardXpToTeam(0, 0.8f, HORDE);

    BattleGround::EndBattleGround(winner);
}

void BattleGroundIC::_SendNodeUpdate(uint8 node)
{
    // Needed to prevent client from bugging
    UpdateWorldState(BG_IC_OP_NODEICONS[node], m_Nodes[node]==0);

    // Send node owner state update to refresh map icons on client
    for (uint8 j = 0; j < 4; j++)
        UpdateWorldState(BG_IC_NodeWorldStates[node][j], m_Nodes[node] == (j + 1));

    if ((node == BG_IC_NODE_QUARRY) || (node == BG_IC_NODE_REFINERY))
    {
        if (m_Nodes[node] >= BG_IC_NODE_TYPE_OCCUPIED)
            m_resource_Timer[node] = BG_IC_RESOURCE_TICK_TIMER;
    }
}

void BattleGroundIC::EventPlayerClickedOnFlag(Player *source, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint8 event = (sBattleGroundMgr.GetGameObjectEventIndex(target_obj->GetGUIDLow())).event1;
    if (event >= BG_IC_NODES_MAX)                           // not a node
        return;

    BG_IC_Nodes node = BG_IC_Nodes(event);

    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(source->GetTeam());

    // Check if player really could use this banner, not cheated
    if (!(m_Nodes[node] == 0 || teamIndex == m_Nodes[node] % 2))
        return;

    source->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    uint32 sound = 0;

    // TODO in the following code we should restructure a bit to avoid
    // duplication (or maybe write functions?)
    // If node is neutral, change to contested
    if (m_Nodes[node] == BG_IC_NODE_TYPE_NEUTRAL)
    {
        UpdatePlayerScore(source, SCORE_BASES_ASSAULTED, 1);
        m_prevNodes[node] = m_Nodes[node];
        m_Nodes[node] = teamIndex + 1;
        // create new contested banner
        _CreateBanner(node, BG_IC_NODE_TYPE_CONTESTED, teamIndex, true);
        _SendNodeUpdate(node);
        m_NodeTimers[node] = BG_IC_FLAG_CAPTURING_TIME;

        if (teamIndex == BG_TEAM_ALLIANCE)
            SendMessage2ToAll(LANG_BG_IC_TEAM_ASSAULTED_NODE_2,CHAT_MSG_BG_SYSTEM_ALLIANCE, source, _GetNodeNameId(node), LANG_BG_ALLY);
        else
            SendMessage2ToAll(LANG_BG_IC_TEAM_ASSAULTED_NODE_2,CHAT_MSG_BG_SYSTEM_HORDE, source, _GetNodeNameId(node), LANG_BG_HORDE);

        sound = BG_IC_SOUND_NODE_CLAIMED;
    }
    // If node is contested
    else if ((m_Nodes[node] == BG_IC_NODE_STATUS_ALLY_CONTESTED) || (m_Nodes[node] == BG_IC_NODE_STATUS_HORDE_CONTESTED))
    {
        // If last state is NOT occupied, change node to enemy-contested
        if (m_prevNodes[node] < BG_IC_NODE_TYPE_OCCUPIED)
        {
            UpdatePlayerScore(source, SCORE_BASES_ASSAULTED, 1);
            m_prevNodes[node] = m_Nodes[node];
            m_Nodes[node] = teamIndex + BG_IC_NODE_TYPE_CONTESTED;
            // create new contested banner
            _CreateBanner(node, BG_IC_NODE_TYPE_CONTESTED, teamIndex, true);
            _SendNodeUpdate(node);
            m_NodeTimers[node] = BG_IC_FLAG_CAPTURING_TIME;

            if (teamIndex == BG_TEAM_ALLIANCE)
                SendMessage2ToAll(LANG_BG_IC_NODE_ASSAULTED,CHAT_MSG_BG_SYSTEM_ALLIANCE, source, _GetNodeNameId(node));
            else
                SendMessage2ToAll(LANG_BG_IC_NODE_ASSAULTED,CHAT_MSG_BG_SYSTEM_HORDE, source, _GetNodeNameId(node));
        }
        // If contested, change back to occupied
        else
        {
            UpdatePlayerScore(source, SCORE_BASES_DEFENDED, 1);
            m_prevNodes[node] = m_Nodes[node];
            m_Nodes[node] = teamIndex + BG_IC_NODE_TYPE_OCCUPIED;
            // create new occupied banner
            _CreateBanner(node, BG_IC_NODE_TYPE_OCCUPIED, teamIndex, true);
            _SendNodeUpdate(node);
            m_NodeTimers[node] = 0;

            if (teamIndex == BG_TEAM_ALLIANCE)
                SendMessage2ToAll(LANG_BG_IC_NODE_DEFENDED,CHAT_MSG_BG_SYSTEM_ALLIANCE, source, _GetNodeNameId(node));
            else
                SendMessage2ToAll(LANG_BG_IC_NODE_DEFENDED,CHAT_MSG_BG_SYSTEM_HORDE, source, _GetNodeNameId(node));
        }
        sound = (teamIndex == BG_TEAM_ALLIANCE) ? BG_IC_SOUND_NODE_ASSAULTED_ALLIANCE : BG_IC_SOUND_NODE_ASSAULTED_HORDE;
    }
    // If node is occupied, change to enemy-contested
    else
    {
        UpdatePlayerScore(source, SCORE_BASES_ASSAULTED, 1);
        m_prevNodes[node] = m_Nodes[node];
        m_Nodes[node] = teamIndex + BG_IC_NODE_TYPE_CONTESTED;
        // create new contested banner
        _CreateBanner(node, BG_IC_NODE_TYPE_CONTESTED, teamIndex, true);
        _SendNodeUpdate(node);
        m_NodeTimers[node] = BG_IC_FLAG_CAPTURING_TIME;

        if (teamIndex == BG_TEAM_ALLIANCE)
            SendMessage2ToAll(LANG_BG_IC_NODE_ASSAULTED,CHAT_MSG_BG_SYSTEM_ALLIANCE, source, _GetNodeNameId(node));
        else
            SendMessage2ToAll(LANG_BG_IC_NODE_ASSAULTED,CHAT_MSG_BG_SYSTEM_HORDE, source, _GetNodeNameId(node));

        sound = (teamIndex == BG_TEAM_ALLIANCE) ? BG_IC_SOUND_NODE_ASSAULTED_ALLIANCE : BG_IC_SOUND_NODE_ASSAULTED_HORDE;

        if (node == BG_IC_NODE_HANGAR)
            (teamIndex == BG_TEAM_ALLIANCE ? gunshipHorde : gunshipAlliance)->BuildStopMovePacket(GetBgMap());
    }
    PlaySoundToAll(sound);
}

void BattleGroundIC::EventPlayerDamageGO(Player *player, GameObject* target_obj, uint32 eventId, uint32 doneBy)
{
    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(player->GetTeam());
}

WorldSafeLocsEntry const* BattleGroundIC::GetClosestGraveYard(Player* player)
{
    BattleGroundTeamIndex teamIndex = GetTeamIndexByTeamId(player->GetTeam());

    // Is there any occupied node for this team?
    std::vector<uint8> nodes;
    for (uint8 i = 0; i < BG_IC_NODES_MAX; ++i)
        if (m_Nodes[i] == teamIndex + 3)
            nodes.push_back(i);

    WorldSafeLocsEntry const* good_entry = NULL;
    // If so, select the closest node to place ghost on
    if (!nodes.empty())
    {
        float plr_x = player->GetPositionX();
        float plr_y = player->GetPositionY();

        float mindist = 999999.0f;
        for (uint8 i = 0; i < nodes.size(); ++i)
        {
            WorldSafeLocsEntry const*entry = sWorldSafeLocsStore.LookupEntry( BG_IC_GraveyardIds[nodes[i]] );
            if (!entry)
                continue;
            float dist = (entry->x - plr_x)*(entry->x - plr_x)+(entry->y - plr_y)*(entry->y - plr_y);
            if (mindist > dist)
            {
                mindist = dist;
                good_entry = entry;
            }
        }
        nodes.clear();
    }
    // If not, place ghost on starting location
    if (!good_entry)
        good_entry = sWorldSafeLocsStore.LookupEntry( BG_IC_GraveyardIds[teamIndex+5] );

    return good_entry;
}

Transport* BattleGroundIC::CreateTransport(uint32 goEntry, uint32 period)
{
    Transport* t = new Transport;

    const GameObjectInfo* goinfo = sObjectMgr.GetGameObjectInfo(goEntry);

    if (!goinfo)
    {
        sLog.outErrorDb("Transport ID: %u will not be loaded, gameobject_template missing", goEntry);
        delete t;
        return NULL;
    }

    std::set<uint32> mapsUsed;
    t->m_period = period;

    if (!t->GenerateWaypoints(goinfo->moTransport.taxiPathId, mapsUsed))
        // skip transports with empty waypoints list
    {
        sLog.outErrorDb("Transport (path id %u) path size = 0. Transport ignored, check DBC files or transport GO data0 field.",goinfo->moTransport.taxiPathId);
        delete t;
        return NULL;
    }

    uint32 mapid = t->m_WayPoints[0].mapid;

    float x = t->m_WayPoints[0].x;
    float y = t->m_WayPoints[0].y;
    float z =  t->m_WayPoints[0].z;
    float o = 1;

    // creates the Gameobject
    if (!t->Create(goEntry, mapid, x, y, z, o, GO_ANIMPROGRESS_DEFAULT, 0))
    {
        delete t;
        return NULL;
    }

    //If we someday decide to use the grid to track transports, here:
    t->SetMap(GetBgMap());

    //for (uint8 i = 0; i < 5; i++)
    //    t->AddNPCPassenger(0,(goEntry == GO_HORDE_GUNSHIP ? NPC_HORDE_GUNSHIP_CANNON : NPC_ALLIANCE_GUNSHIP_CANNON), (goEntry == GO_HORDE_GUNSHIP ? hordeGunshipPassengers[i].GetPositionX() : allianceGunshipPassengers[i].GetPositionX()) , (goEntry == GO_HORDE_GUNSHIP ? hordeGunshipPassengers[i].GetPositionY() : allianceGunshipPassengers[i].GetPositionY()),(goEntry == GO_HORDE_GUNSHIP ? hordeGunshipPassengers[i].GetPositionZ() : allianceGunshipPassengers[i].GetPositionZ()), (goEntry == GO_HORDE_GUNSHIP ? hordeGunshipPassengers[i].GetOrientation() : allianceGunshipPassengers[i].GetOrientation()));

    return t;
}

void BattleGroundIC::HandleBuffs()
{
    for (BattleGroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
    {
        if (Player *plr = sObjectMgr.GetPlayer(itr->first))
        {
            // quarry / refinery buffs
            for (uint8 node = BG_IC_NODE_QUARRY; node <= BG_IC_NODE_REFINERY; node++)
            {
                if (m_Nodes[node] >= BG_IC_NODE_TYPE_OCCUPIED)
                {
                    if ((node == BG_IC_NODE_QUARRY) && (!plr->HasAura(SPELL_QUARRY)) && (plr->GetTeam() == (m_Nodes[node] - BG_IC_NODE_TYPE_OCCUPIED == 0 ? ALLIANCE : HORDE)))
                        plr->CastSpell(plr, SPELL_QUARRY, true);
                    else if ((node == BG_IC_NODE_REFINERY) && (!plr->HasAura(SPELL_REFINERY)) && (plr->GetTeam() == (m_Nodes[node] - BG_IC_NODE_TYPE_OCCUPIED == 0 ? ALLIANCE : HORDE)))
                        plr->CastSpell(plr, SPELL_REFINERY, true);
                }
                else
                {
                    if (node == BG_IC_NODE_QUARRY)
                        plr->RemoveAurasDueToSpell(SPELL_QUARRY);
                    else 
                        plr->RemoveAurasDueToSpell(SPELL_REFINERY);
                }
            }
            // parachute handling
            if (!plr->IsFalling())
                continue;

            float height = plr->GetPositionZ();
            if (height < 180 && height > 140 && (!plr->HasAura(SPELL_PARACHUTE)))
                plr->CastSpell(plr, SPELL_PARACHUTE, true);
        }
    }
}

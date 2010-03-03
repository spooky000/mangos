/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
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
#include "BattleGroundSA.h"
#include "Language.h"
#include "WorldPacket.h"

BattleGroundSA::BattleGroundSA()
{
    m_StartMessageIds[BG_STARTING_EVENT_FIRST]  = LANG_BG_SA_START_TWO_MINUTES;
    m_StartMessageIds[BG_STARTING_EVENT_SECOND] = LANG_BG_SA_START_ONE_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_THIRD]  = LANG_BG_SA_START_HALF_MINUTE;
    m_StartMessageIds[BG_STARTING_EVENT_FOURTH] = LANG_BG_SA_HAS_BEGUN;

    TimerEnabled = false;
}

BattleGroundSA::~BattleGroundSA()
{

}

void BattleGroundSA::Update(uint32 diff)
{
    BattleGround::Update(diff);
    TotalTime += diff;

    if(status == BG_SA_WARMUP || status == BG_SA_SECOND_WARMUP)
    {
        if(TotalTime >= BG_SA_WARMUPLENGTH)
        {
            TotalTime = 0;
            ToggleTimer();
            status = (status == BG_SA_WARMUP) ? BG_SA_ROUND_ONE : BG_SA_ROUND_TWO;
        }
        if(TotalTime >= BG_SA_BOAT_START)
            //StartShips();
            return;
    }
    else if(status == BG_SA_ROUND_ONE)
    {
        if(TotalTime >= BG_SA_ROUNDLENGTH)
        {
            RoundScores[0].time = TotalTime;
            TotalTime = 0;
            status = BG_SA_SECOND_WARMUP;
            attackers = (attackers == BG_TEAM_ALLIANCE) ? BG_TEAM_HORDE : BG_TEAM_ALLIANCE;
            RoundScores[0].winner = attackers;
            status = BG_SA_SECOND_WARMUP;
            ToggleTimer();
            //ResetObjs();
            return;
        }
    }
    else if(status == BG_SA_ROUND_TWO)
    {
        if(TotalTime >= BG_SA_ROUNDLENGTH)
        {
            RoundScores[1].time = TotalTime;
            RoundScores[1].winner = (attackers == BG_TEAM_ALLIANCE) ? BG_TEAM_HORDE : BG_TEAM_ALLIANCE;

            if(RoundScores[0].time < RoundScores[1].time)
                EndBattleGround(RoundScores[0].winner == BG_TEAM_ALLIANCE ? ALLIANCE : HORDE);
            else
                EndBattleGround(RoundScores[1].winner == BG_TEAM_ALLIANCE ? ALLIANCE : HORDE);

            return;
        }
    }

    if(status == BG_SA_ROUND_ONE || status == BG_SA_ROUND_TWO)
        SendTime();
}

void BattleGroundSA::FillInitialWorldStates(WorldPacket& data)
{
  uint32 ally_attacks  = uint32(attackers == BG_TEAM_ALLIANCE ? 1 : 0);
  uint32 horde_attacks = uint32(attackers == BG_TEAM_HORDE ? 1 : 0);

  data << uint32(BG_SA_ANCIENT_GATEWS)      << uint32(GateStatus[BG_SA_ANCIENT_GATE]);
  data << uint32(BG_SA_YELLOW_GATEWS)       << uint32(GateStatus[BG_SA_YELLOW_GATE]);
  data << uint32(BG_SA_GREEN_GATEWS)        << uint32(GateStatus[BG_SA_GREEN_GATE]);
  data << uint32(BG_SA_BLUE_GATEWS)         << uint32(GateStatus[BG_SA_BLUE_GATE]);
  data << uint32(BG_SA_RED_GATEWS)          << uint32(GateStatus[BG_SA_RED_GATE]);
  data << uint32(BG_SA_PURPLE_GATEWS)       << uint32(GateStatus[BG_SA_PURPLE_GATE]);

  data << uint32(BG_SA_BONUS_TIMER)         << uint32(0);

  data << uint32(BG_SA_HORDE_ATTACKS)       << horde_attacks;
  data << uint32(BG_SA_ALLY_ATTACKS)        << ally_attacks;

  //Time will be sent on first update...
  data << uint32(BG_SA_ENABLE_TIMER)        << ((TimerEnabled) ? uint32(1) : uint32(0));
  data << uint32(BG_SA_TIMER_MINS)          << uint32(0);
  data << uint32(BG_SA_TIMER_SEC_TENS)      << uint32(0);
  data << uint32(BG_SA_TIMER_SEC_DECS)      << uint32(0);

  data << uint32(BG_SA_RIGHT_GY_HORDE)      << uint32(GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY]    == BG_TEAM_HORDE?1:0 );
  data << uint32(BG_SA_LEFT_GY_HORDE)       << uint32(GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY]     == BG_TEAM_HORDE?1:0 );
  data << uint32(BG_SA_CENTER_GY_HORDE)     << uint32(GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY]  == BG_TEAM_HORDE?1:0 );

  data << uint32(BG_SA_RIGHT_GY_ALLIANCE)   << uint32(GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY]    == BG_TEAM_ALLIANCE?1:0 );
  data << uint32(BG_SA_LEFT_GY_ALLIANCE)    << uint32(GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY]     == BG_TEAM_ALLIANCE?1:0 );
  data << uint32(BG_SA_CENTER_GY_ALLIANCE)  << uint32(GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY]  == BG_TEAM_ALLIANCE?1:0 );

  data << uint32(BG_SA_HORDE_DEFENCE_TOKEN) << ally_attacks;
  data << uint32(BG_SA_ALLIANCE_DEFENCE_TOKEN) << horde_attacks;

  data << uint32(BG_SA_LEFT_ATT_TOKEN_HRD)  << horde_attacks;
  data << uint32(BG_SA_RIGHT_ATT_TOKEN_HRD) << horde_attacks;
  data << uint32(BG_SA_RIGHT_ATT_TOKEN_ALL) << ally_attacks;
  data << uint32(BG_SA_LEFT_ATT_TOKEN_ALL)  << ally_attacks;
}

void BattleGroundSA::StartingEventCloseDoors()
{
}

void BattleGroundSA::StartingEventOpenDoors()
{
}

void BattleGroundSA::AddPlayer(Player *plr)
{
    BattleGround::AddPlayer(plr);
    //create score and add it to map, default values are set in constructor
    BattleGroundSAScore* sc = new BattleGroundSAScore;

    if(!ShipsStarted)
    {
        if(plr->GetBGTeam() == attackers)
        {
            plr->CastSpell(plr,12438,true);//Without this player falls before boat loads...

            if(urand(0,1))
                plr->TeleportTo(607, 2682.936f, -830.368f, 50.0f, 2.895f, 0);
            else
                plr->TeleportTo(607, 2577.003f, 980.261f, 50.0f, 0.807f, 0);

        }
        else
            plr->TeleportTo(607, 1209.7f, -65.16f, 70.1f, 0.0f, 0);
    }
    else
    {
        if(plr->GetBGTeam() == attackers)
            plr->TeleportTo(607, 1600.381f, -106.263f, 8.8745f, 3.78f, 0);
        else
            plr->TeleportTo(607, 1209.7f, -65.16f, 70.1f, 0.0f, 0);
    }

    m_PlayerScores[plr->GetGUID()] = sc;
}

void BattleGroundSA::RemovePlayer(Player* /*plr*/,uint64 /*guid*/)
{

}

void BattleGroundSA::HandleAreaTrigger(Player * /*Source*/, uint32 /*Trigger*/)
{
    // this is wrong way to implement these things. On official it done by gameobject spell cast.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;
}

void BattleGroundSA::HandleKillUnit(Creature* unit, Player* killer)
{
    if(!unit)
        return;

    if(unit->GetEntry() == 28781)  //Demolisher
        UpdatePlayerScore(killer, SCORE_DESTROYED_DEMOLISHER, 1);
}

void BattleGroundSA::UpdatePlayerScore(Player* Source, uint32 type, uint32 value)
{

    BattleGroundScoreMap::iterator itr = m_PlayerScores.find(Source->GetGUID());
    if(itr == m_PlayerScores.end())                         // player not found...
        return;

    if(type == SCORE_DESTROYED_DEMOLISHER)
        ((BattleGroundSAScore*)itr->second)->demolishers_destroyed += value;
    else if(type == SCORE_DESTROYED_WALL)
        ((BattleGroundSAScore*)itr->second)->gates_destroyed += value;
    else
        BattleGround::UpdatePlayerScore(Source,type,value);
}

void BattleGroundSA::EndBattleGround(uint32 winner)
{
    //honor reward for winning
    if (winner == ALLIANCE)
        RewardHonorToTeam(GetBonusHonorFromKill(BG_SA_HONOR_WIN), ALLIANCE);
    else if (winner == HORDE)
        RewardHonorToTeam(GetBonusHonorFromKill(BG_SA_HONOR_WIN), HORDE);
    
    //complete map_end rewards (even if no team wins)
    RewardHonorToTeam(GetBonusHonorFromKill(BG_SA_HONOR_END), ALLIANCE);
    RewardHonorToTeam(GetBonusHonorFromKill(BG_SA_HONOR_END), HORDE);

    BattleGround::EndBattleGround(winner);
}

void BattleGroundSA::Reset()
{
    TotalTime = 0;
    attackers = ( (urand(0,1)) ? BG_TEAM_ALLIANCE : BG_TEAM_HORDE);
    for(uint8 i = 0; i <= 5; i++)
    {
        GateStatus[i] = BG_SA_GATE_OK;
    }
    ShipsStarted = false;
    status = BG_SA_WARMUP;
}

void BattleGroundSA::SendTime()
{
    uint32 end_of_round = (BG_SA_ROUNDLENGTH - TotalTime);
    UpdateWorldState(BG_SA_TIMER_MINS, end_of_round/60000);
    UpdateWorldState(BG_SA_TIMER_SEC_TENS, (end_of_round%60000)/10000);
    UpdateWorldState(BG_SA_TIMER_SEC_DECS, ((end_of_round%60000)%10000)/1000);
}

void BattleGroundSA::ToggleTimer()
{
    TimerEnabled = !TimerEnabled;
    UpdateWorldState(BG_SA_ENABLE_TIMER, (TimerEnabled) ? 1 : 0);
}

void BattleGroundSA::EventPlayerClickedOnFlag(Player *Source, GameObject* target_obj)
{
    switch(target_obj->GetEntry())
    {
    case 191307:
    case 191308:
        CaptureGraveyard(BG_SA_LEFT_CAPTURABLE_GY);
      break;
    case 191305:
    case 191306:
        CaptureGraveyard(BG_SA_RIGHT_CAPTURABLE_GY);
      break;
    case 191310:
    case 191309:
        CaptureGraveyard(BG_SA_CENTRAL_CAPTURABLE_GY);
      break;
    default:
        return;
    };
}

void BattleGroundSA::CaptureGraveyard(BG_SA_Graveyards i)
{
    //DelCreature(BG_SA_MAXNPC + i);
    GraveyardStatus[i] = (GraveyardStatus[i] == BG_TEAM_ALLIANCE? BG_TEAM_HORDE : BG_TEAM_ALLIANCE);
    WorldSafeLocsEntry const *sg = NULL;
    sg = sWorldSafeLocsStore.LookupEntry(BG_SA_GYEntries[i]);
    //AddSpiritGuide(i + BG_SA_NPC_MAX, sg->x, sg->y, sg->z, BG_SA_GYOrientation[i], (GraveyardStatus[i] == BG_TEAM_ALLIANCE?  ALLIANCE : HORDE ));
    uint32 npc = 0;

    switch(i)
    {
    case BG_SA_LEFT_CAPTURABLE_GY:
            SpawnBGObject(BG_SA_LEFT_FLAG,RESPAWN_ONE_DAY);
            npc = BG_SA_NPC_RIGSPARK;
            /*AddCreature(BG_SA_NpcEntries[npc], npc, attackers, 
            BG_SA_NpcSpawnlocs[npc][0], BG_SA_NpcSpawnlocs[npc][1],
            BG_SA_NpcSpawnlocs[npc][2], BG_SA_NpcSpawnlocs[npc][3]);*/
            UpdateWorldState(BG_SA_LEFT_GY_ALLIANCE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 1:0));
            UpdateWorldState(BG_SA_LEFT_GY_HORDE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 0:1));
        break;
    case BG_SA_RIGHT_CAPTURABLE_GY:
            SpawnBGObject(BG_SA_RIGHT_FLAG, RESPAWN_ONE_DAY);
            npc = BG_SA_NPC_SPARKLIGHT;
            /*AddCreature(BG_SA_NpcEntries[npc], npc, attackers, 
            BG_SA_NpcSpawnlocs[npc][0], BG_SA_NpcSpawnlocs[npc][1],
            BG_SA_NpcSpawnlocs[npc][2], BG_SA_NpcSpawnlocs[npc][3]);*/
            UpdateWorldState(BG_SA_RIGHT_GY_ALLIANCE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 1:0));
            UpdateWorldState(BG_SA_RIGHT_GY_HORDE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 0:1));
        break;
    case BG_SA_CENTRAL_CAPTURABLE_GY:
            SpawnBGObject(BG_SA_CENTRAL_FLAG, RESPAWN_ONE_DAY);
            UpdateWorldState(BG_SA_CENTER_GY_ALLIANCE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 1:0));
            UpdateWorldState(BG_SA_CENTER_GY_HORDE, (GraveyardStatus[i] == BG_TEAM_ALLIANCE? 0:1));
        break;
    default:
            ASSERT(0);
        break;
    };
}
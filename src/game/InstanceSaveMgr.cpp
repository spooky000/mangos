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

#include "InstanceSaveMgr.h"

#include "SQLStorages.h"
#include "Player.h"
#include "GridNotifiers.h"
#include "Log.h"
#include "GridStates.h"
#include "CellImpl.h"
#include "Map.h"
#include "MapManager.h"
#include "Timer.h"
#include "GridNotifiersImpl.h"
#include "Transports.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Group.h"
#include "InstanceData.h"
#include "ProgressBar.h"

INSTANTIATE_SINGLETON_1( InstanceSaveManager );

static uint32 resetEventTypeDelay[MAX_RESET_EVENT_TYPE] = { 0, 3600, 900, 300, 60 };

//== InstanceSave functions ================================

InstanceSave::InstanceSave(uint16 MapId, uint32 InstanceId, Difficulty difficulty, time_t resetTime, bool canReset)
: m_resetTime(resetTime), m_instanceid(InstanceId), m_mapid(MapId),
  m_difficulty(difficulty), m_canReset(canReset), m_usedByMap(false)
{
}

InstanceSave::~InstanceSave()
{
    while(!m_playerList.empty())
    {
        Player *player = *(m_playerList.begin());
        player->UnbindInstance(GetMapId(), GetDifficulty(), true);
    }
    while(!m_groupList.empty())
    {
        Group *group = *(m_groupList.begin());
        group->UnbindInstance(GetMapId(), GetDifficulty(), true);
    }
}

/*
    Called from AddInstanceSave
*/
void InstanceSave::SaveToDB()
{
    // save instance data too
    std::string data;

    if (Map *map = sMapMgr.FindMap(GetMapId(),m_instanceid))
    {
        InstanceData *iData = map->GetInstanceData();
        if(iData && iData->Save())
        {
            data = iData->Save();
            CharacterDatabase.escape_string(data);
        }
    }

    if (m_instanceid)
        CharacterDatabase.PExecute("INSERT INTO instance VALUES ('%u', '%u', '"UI64FMTD"', '%u', '%s')", m_instanceid, GetMapId(), (uint64)GetResetTimeForDB(), GetDifficulty(), data.c_str());
}

time_t InstanceSave::GetResetTimeForDB() const
{
    // only save the reset time for normal instances
    const MapEntry *entry = sMapStore.LookupEntry(GetMapId());
    if(!entry || entry->map_type == MAP_RAID || GetDifficulty() == DUNGEON_DIFFICULTY_HEROIC)
        return 0;
    else
        return GetResetTime();
}

// to cache or not to cache, that is the question
InstanceTemplate const* InstanceSave::GetTemplate() const
{
    return ObjectMgr::GetInstanceTemplate(m_mapid);
}

MapEntry const* InstanceSave::GetMapEntry() const
{
    return sMapStore.LookupEntry(m_mapid);
}

void InstanceSave::DeleteFromDB()
{
    if (GetInstanceId())
        InstanceSaveManager::DeleteInstanceFromDB(GetInstanceId());
}

void InstanceSave::DeleteRespawnTimes()
{
    // possible reset for instanceable map only
    if (!m_instanceid)
        return;

    m_goRespawnTimes.clear();
    m_creatureRespawnTimes.clear();

    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute("DELETE FROM creature_respawn WHERE instance = '%u'", m_instanceid);
    CharacterDatabase.PExecute("DELETE FROM gameobject_respawn WHERE instance = '%u'", m_instanceid);
    CharacterDatabase.CommitTransaction();
}

/* true if the instance save is still valid */
bool InstanceSave::UnloadIfEmpty()
{
    // prevent unload if any bounded groups or online bounded player still exists
    // also prevent unload if respawn data still exist (will not prevent reset by scheduler)
    if (m_playerList.empty() && m_groupList.empty() && !m_usedByMap &&
        m_creatureRespawnTimes.empty() && m_goRespawnTimes.empty())
    {
        sInstanceSaveMgr.RemoveInstanceSave(GetMapId(), GetInstanceId());
        return false;
    }
    else
        return true;
}

void InstanceSave::SaveCreatureRespawnTime(uint32 loguid, time_t t)
{
    SetCreatureRespawnTime(loguid, t);

    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute("DELETE FROM creature_respawn WHERE guid = '%u' AND instance = '%u'", loguid, m_instanceid);
    if(t > sWorld.GetGameTime())
        CharacterDatabase.PExecute("INSERT INTO creature_respawn VALUES ( '%u', '" UI64FMTD "', '%u' )", loguid, uint64(t), m_instanceid);
    CharacterDatabase.CommitTransaction();
}

void InstanceSave::SaveGORespawnTime(uint32 loguid, time_t t)
{
    SetGORespawnTime(loguid, t);

    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute("DELETE FROM gameobject_respawn WHERE guid = '%u' AND instance = '%u'", loguid, m_instanceid);
    if(t > sWorld.GetGameTime())
        CharacterDatabase.PExecute("INSERT INTO gameobject_respawn VALUES ( '%u', '" UI64FMTD "', '%u' )", loguid, uint64(t), m_instanceid);
    CharacterDatabase.CommitTransaction();
}

void InstanceSave::SetCreatureRespawnTime( uint32 loguid, time_t t )
{
    if (t > sWorld.GetGameTime())
        m_creatureRespawnTimes[loguid] = t;
    else
    {
        m_creatureRespawnTimes.erase(loguid);
        UnloadIfEmpty();
    }
}

void InstanceSave::SetGORespawnTime( uint32 loguid, time_t t )
{
    if (t > sWorld.GetGameTime())
        m_goRespawnTimes[loguid] = t;
    else
    {
        m_goRespawnTimes.erase(loguid);
        UnloadIfEmpty();
    }
}

//== InstanceResetScheduler functions ======================

uint32 InstanceResetScheduler::GetMaxResetTimeFor(MapDifficulty const* mapDiff)
{
    if (!mapDiff || !mapDiff->resetTime)
        return 0;

    uint32 delay = uint32(mapDiff->resetTime / DAY * sWorld.getConfig(CONFIG_FLOAT_RATE_INSTANCE_RESET_TIME)) * DAY;

    if (delay < DAY)                                        // the reset_delay must be at least one day
        delay = DAY;

    return delay;
}

time_t InstanceResetScheduler::CalculateNextResetTime(MapDifficulty const* mapDiff, time_t prevResetTime)
{
    uint32 diff = sWorld.getConfig(CONFIG_UINT32_INSTANCE_RESET_TIME_HOUR) * HOUR;
    uint32 period = GetMaxResetTimeFor(mapDiff);
    return ((prevResetTime + MINUTE) / DAY * DAY) + period + diff;
}

void InstanceResetScheduler::LoadResetTimes()
{
    time_t now = time(NULL);
    time_t today = (now / DAY) * DAY;

    // NOTE: Use DirectPExecute for tables that will be queried later

    // get the current reset times for normal instances (these may need to be updated)
    // these are only kept in memory for InstanceSaves that are loaded later
    // resettime = 0 in the DB for raid/heroic instances so those are skipped
    typedef std::pair<uint32 /*PAIR32(map,difficulty)*/, time_t> ResetTimeMapDiffType;
    typedef std::map<uint32, ResetTimeMapDiffType> InstResetTimeMapDiffType;
    InstResetTimeMapDiffType instResetTime;

    QueryResult *result = CharacterDatabase.Query("SELECT id, map, difficulty, resettime FROM instance WHERE resettime > 0");
    if( result )
    {
        do
        {
            if(time_t resettime = time_t((*result)[3].GetUInt64()))
            {
                uint32 id = (*result)[0].GetUInt32();
                uint32 mapid = (*result)[1].GetUInt32();
                uint32 difficulty = (*result)[2].GetUInt32();
                instResetTime[id] = ResetTimeMapDiffType(MAKE_PAIR32(mapid,difficulty), resettime);
            }
        }
        while (result->NextRow());
        delete result;

        // update reset time for normal instances with the max creature respawn time + X hours
        result = CharacterDatabase.Query("SELECT MAX(respawntime), instance FROM creature_respawn WHERE instance > 0 GROUP BY instance");
        if( result )
        {
            do
            {
                Field *fields = result->Fetch();
                uint32 instance = fields[1].GetUInt32();
                time_t resettime = time_t(fields[0].GetUInt64() + 2 * HOUR);
                InstResetTimeMapDiffType::iterator itr = instResetTime.find(instance);
                if(itr != instResetTime.end() && itr->second.second != resettime)
                {
                    CharacterDatabase.DirectPExecute("UPDATE instance SET resettime = '"UI64FMTD"' WHERE id = '%u'", uint64(resettime), instance);
                    itr->second.second = resettime;
                }
            }
            while (result->NextRow());
            delete result;
        }

        // schedule the reset times
        for(InstResetTimeMapDiffType::iterator itr = instResetTime.begin(); itr != instResetTime.end(); ++itr)
            if(itr->second.second > now)
                ScheduleReset(true, itr->second.second, InstanceResetEvent(RESET_EVENT_DUNGEON, PAIR32_LOPART(itr->second.first),Difficulty(PAIR32_HIPART(itr->second.first)),itr->first));
    }

    // load the global respawn times for raid/heroic instances
    uint32 diff = sWorld.getConfig(CONFIG_UINT32_INSTANCE_RESET_TIME_HOUR) * HOUR;
    result = CharacterDatabase.Query("SELECT mapid, difficulty, resettime FROM instance_reset");
    if(result)
    {
        do
        {
            Field *fields = result->Fetch();
            uint32 mapid = fields[0].GetUInt32();
            Difficulty difficulty = Difficulty(fields[1].GetUInt32());
            uint64 oldresettime = fields[2].GetUInt64();

            MapDifficulty const* mapDiff = GetMapDifficultyData(mapid,difficulty);
            if(!mapDiff)
            {
                sLog.outError("InstanceSaveManager::LoadResetTimes: invalid mapid(%u)/difficulty(%u) pair in instance_reset!", mapid, difficulty);
                CharacterDatabase.DirectPExecute("DELETE FROM instance_reset WHERE mapid = '%u' AND difficulty = '%u'", mapid,difficulty);
                continue;
            }

            // update the reset time if the hour in the configs changes
            uint64 newresettime = (oldresettime / DAY) * DAY + diff;
            if(oldresettime != newresettime)
                CharacterDatabase.DirectPExecute("UPDATE instance_reset SET resettime = '"UI64FMTD"' WHERE mapid = '%u' AND difficulty = '%u'", newresettime, mapid, difficulty);

            SetResetTimeFor(mapid,difficulty,newresettime);
        } while(result->NextRow());
        delete result;
    }

    // clean expired instances, references to them will be deleted in CleanupInstances
    // must be done before calculating new reset times
    m_InstanceSaves._CleanupExpiredInstancesAtTime(now);

    // calculate new global reset times for expired instances and those that have never been reset yet
    // add the global reset times to the priority queue
    for(MapDifficultyMap::const_iterator itr = sMapDifficultyMap.begin(); itr != sMapDifficultyMap.end(); ++itr)
    {
        uint32 map_diff_pair = itr->first;
        uint32 mapid = PAIR32_LOPART(map_diff_pair);
        Difficulty difficulty = Difficulty(PAIR32_HIPART(map_diff_pair));
        MapDifficulty const* mapDiff = &itr->second;

        // skip mapDiff without global reset time
        if (!mapDiff->resetTime)
            continue;

        uint32 period = GetMaxResetTimeFor(mapDiff);
        time_t t = GetResetTimeFor(mapid,difficulty);
        if(!t)
        {
            // initialize the reset time
            t = today + period + diff;
            CharacterDatabase.DirectPExecute("INSERT INTO instance_reset VALUES ('%u','%u','"UI64FMTD"')", mapid, difficulty, (uint64)t);
        }

        if(t < now)
        {
            // assume that expired instances have already been cleaned
            // calculate the next reset time
            t = (t / DAY) * DAY;
            t += ((today - t) / period + 1) * period + diff;
            CharacterDatabase.DirectPExecute("UPDATE instance_reset SET resettime = '"UI64FMTD"' WHERE mapid = '%u' AND difficulty= '%u'", (uint64)t, mapid, difficulty);
        }

        SetResetTimeFor(mapid,difficulty,t);

        // schedule the global reset/warning
        ResetEventType type = RESET_EVENT_INFORM_1;
        for(; type < RESET_EVENT_INFORM_LAST; type = ResetEventType(type+1))
            if(t - resetEventTypeDelay[type] > now)
                break;

        ScheduleReset(true, t - resetEventTypeDelay[type], InstanceResetEvent(type, mapid, difficulty, 0));
    }
}

void InstanceResetScheduler::ScheduleReset(bool add, time_t time, InstanceResetEvent event)
{
    if (add)
        m_resetTimeQueue.insert(std::pair<time_t, InstanceResetEvent>(time, event));
    else
    {
        // find the event in the queue and remove it
        ResetTimeQueue::iterator itr;
        std::pair<ResetTimeQueue::iterator, ResetTimeQueue::iterator> range;
        range = m_resetTimeQueue.equal_range(time);
        for(itr = range.first; itr != range.second; ++itr)
        {
            if (itr->second == event)
            {
                m_resetTimeQueue.erase(itr);
                return;
            }
        }
        // in case the reset time changed (should happen very rarely), we search the whole queue
        if(itr == range.second)
        {
            for(itr = m_resetTimeQueue.begin(); itr != m_resetTimeQueue.end(); ++itr)
            {
                if(itr->second == event)
                {
                    m_resetTimeQueue.erase(itr);
                    return;
                }
            }

            if(itr == m_resetTimeQueue.end())
                sLog.outError("InstanceResetScheduler::ScheduleReset: cannot cancel the reset, the event(%d,%d,%d) was not found!", event.type, event.mapid, event.instanceId);
        }
    }
}

void InstanceResetScheduler::Update()
{
    time_t now = time(NULL), t;
    while(!m_resetTimeQueue.empty() && (t = m_resetTimeQueue.begin()->first) < now)
    {
        InstanceResetEvent &event = m_resetTimeQueue.begin()->second;
        if (event.type == RESET_EVENT_DUNGEON)
        {
            // for individual normal instances, max creature respawn + X hours
            m_InstanceSaves._ResetInstance(event.mapid, event.instanceId);
        }
        else
        {
            // global reset/warning for a certain map
            time_t resetTime = GetResetTimeFor(event.mapid,event.difficulty);
            m_InstanceSaves._ResetOrWarnAll(event.mapid, event.difficulty, event.type != RESET_EVENT_INFORM_LAST, uint32(resetTime - now));
            if (event.type != RESET_EVENT_INFORM_LAST)
            {
                // schedule the next warning/reset
                event.type = ResetEventType(event.type+1);
                ScheduleReset(true, resetTime - resetEventTypeDelay[event.type], event);
            }
            else
            {
                // re-schedule the next/new global reset/warning
                // calculate the next reset time
                MapDifficulty const* mapDiff = GetMapDifficultyData(event.mapid,event.difficulty);
                MANGOS_ASSERT(mapDiff);

                time_t next_reset = InstanceResetScheduler::CalculateNextResetTime(mapDiff, resetTime);

                CharacterDatabase.DirectPExecute("UPDATE instance_reset SET resettime = '"UI64FMTD"' WHERE mapid = '%u' AND difficulty = '%u'", uint64(next_reset), uint32(event.mapid), uint32(event.difficulty));

                SetResetTimeFor(event.mapid, event.difficulty, next_reset);

                ResetEventType type = RESET_EVENT_INFORM_1;
                for (; type < RESET_EVENT_INFORM_LAST; type = ResetEventType(type+1))
                    if (next_reset - resetEventTypeDelay[type] > now)
                        break;

                // add new scheduler event to the queue
                event.type = type;
                ScheduleReset(true, next_reset - resetEventTypeDelay[event.type], event);
            }
        }
        m_resetTimeQueue.erase(m_resetTimeQueue.begin());
    }
}

//== InstanceSaveManager functions =========================

InstanceSaveManager::InstanceSaveManager() : lock_instLists(false), m_Scheduler(*this)
{
}

InstanceSaveManager::~InstanceSaveManager()
{
    // it is undefined whether this or objectmgr will be unloaded first
    // so we must be prepared for both cases
    lock_instLists = true;
    for (InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.begin(); itr != m_instanceSaveByInstanceId.end(); ++itr)
        delete  itr->second;
    for (InstanceSaveHashMap::iterator itr = m_instanceSaveByMapId.begin(); itr != m_instanceSaveByMapId.end(); ++itr)
        delete  itr->second;
}

/*
- adding instance into manager
- called from InstanceMap::Add, _LoadBoundInstances, LoadGroups
*/
InstanceSave* InstanceSaveManager::AddInstanceSave(uint32 mapId, uint32 instanceId, Difficulty difficulty, time_t resetTime, bool canReset, bool load)
{
    if(InstanceSave *old_save = GetInstanceSave(mapId, instanceId))
        return old_save;

    const MapEntry* entry = sMapStore.LookupEntry(mapId);
    if (!entry)
    {
        sLog.outError("InstanceSaveManager::AddInstanceSave: wrong mapid = %d, instanceid = %d!", mapId, instanceId);
        return NULL;
    }

    if (entry->Instanceable())
    {
        if (instanceId == 0)
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, wrong instanceid = %d for instanceable map!", mapId, instanceId);
            return NULL;
        }

        if (difficulty >= (entry->IsRaid() ? MAX_RAID_DIFFICULTY : MAX_DUNGEON_DIFFICULTY))
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, instanceid = %d, wrong difficulty %u!", mapId, instanceId, difficulty);
            return NULL;
        }

        if (!resetTime)
        {
            // initialize reset time
            // for normal instances if no creatures are killed the instance will reset in two hours
            if (entry->map_type == MAP_RAID || difficulty > DUNGEON_DIFFICULTY_NORMAL)
                resetTime = m_Scheduler.GetResetTimeFor(mapId,difficulty);
            else
            {
                resetTime = time(NULL) + 2 * HOUR;
                // normally this will be removed soon after in InstanceMap::Add, prevent error
                m_Scheduler.ScheduleReset(true, resetTime, InstanceResetEvent(RESET_EVENT_DUNGEON, mapId, difficulty, instanceId));
            }
        }
    }
    else
    {
        if (instanceId != 0)
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, wrong instanceid = %d for non-instanceable map!", mapId, instanceId);
            return NULL;
        }

        if (difficulty != REGULAR_DIFFICULTY)
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, instanceid = %d, wrong difficulty %u  for non-instanceable map!", mapId, instanceId, difficulty);
            return NULL;
        }

        if (resetTime)
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, instanceid = %d, wrong reset time %u  for non-instanceable map!", mapId, instanceId, resetTime);
            return NULL;
        }

        if (canReset)
        {
            sLog.outError("InstanceSaveManager::AddInstanceSave: mapid = %d, instanceid = %d, wrong canReset %u  for non-instanceable map!", mapId, instanceId, canReset ? 1 : 0);
            return NULL;
        }
    }

    DEBUG_LOG("InstanceSaveManager::AddInstanceSave: mapid = %d, instanceid = %d, reset time = %u, canRset = %u", mapId, instanceId, resetTime, canReset ? 1 : 0);

    InstanceSave *save = new InstanceSave(mapId, instanceId, difficulty, resetTime, canReset);
    if (!load)
        save->SaveToDB();

    if (entry->Instanceable())
        m_instanceSaveByInstanceId[instanceId] = save;
    else
        m_instanceSaveByMapId[mapId] = save;

    return save;
}

InstanceSave *InstanceSaveManager::GetInstanceSave(uint32 mapId, uint32 instanceId)
{
    if (instanceId)
    {
        InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.find(instanceId);
        return itr != m_instanceSaveByInstanceId.end() ? itr->second : NULL;
    }
    else
    {
        InstanceSaveHashMap::iterator itr = m_instanceSaveByMapId.find(mapId);
        return itr != m_instanceSaveByMapId.end() ? itr->second : NULL;
    }
}

void InstanceSaveManager::DeleteInstanceFromDB(uint32 instanceid)
{
    if (instanceid)
    {
        CharacterDatabase.BeginTransaction();
        CharacterDatabase.PExecute("DELETE FROM instance WHERE id = '%u'", instanceid);
        CharacterDatabase.PExecute("DELETE FROM character_instance WHERE instance = '%u'", instanceid);
        CharacterDatabase.PExecute("DELETE FROM group_instance WHERE instance = '%u'", instanceid);
        CharacterDatabase.PExecute("DELETE FROM creature_respawn WHERE instance = '%u'", instanceid);
        CharacterDatabase.PExecute("DELETE FROM gameobject_respawn WHERE instance = '%u'", instanceid);
        CharacterDatabase.CommitTransaction();
    }
}

void InstanceSaveManager::RemoveInstanceSave(uint32 mapId, uint32 instanceId)
{
    if (lock_instLists)
        return;

    if (instanceId)
    {
        InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.find(instanceId);
        if (itr != m_instanceSaveByInstanceId.end())
        {
            // save the resettime for normal instances only when they get unloaded
            if(time_t resettime = itr->second->GetResetTimeForDB())
                CharacterDatabase.PExecute("UPDATE instance SET resettime = '"UI64FMTD"' WHERE id = '%u'", (uint64)resettime, instanceId);

            _ResetSave(m_instanceSaveByInstanceId, itr);
        }
    }
    else
    {
        InstanceSaveHashMap::iterator itr = m_instanceSaveByMapId.find(mapId);
        if (itr != m_instanceSaveByMapId.end())
            _ResetSave(m_instanceSaveByMapId, itr);
    }
}

void InstanceSaveManager::_DelHelper(DatabaseType &db, const char *fields, const char *table, const char *queryTail,...)
{
    Tokens fieldTokens(fields, ',');
    MANGOS_ASSERT(fieldTokens.size() != 0);

    va_list ap;
    char szQueryTail [MAX_QUERY_LEN];
    va_start(ap, queryTail);
    vsnprintf( szQueryTail, MAX_QUERY_LEN, queryTail, ap );
    va_end(ap);

    QueryResult *result = db.PQuery("SELECT %s FROM %s %s", fields, table, szQueryTail);
    if(result)
    {
        do
        {
            Field *fields = result->Fetch();
            std::ostringstream ss;
            for(size_t i = 0; i < fieldTokens.size(); i++)
            {
                std::string fieldValue = fields[i].GetCppString();
                db.escape_string(fieldValue);
                ss << (i != 0 ? " AND " : "") << fieldTokens[i] << " = '" << fieldValue << "'";
            }
            db.PExecute("DELETE FROM %s WHERE %s", table, ss.str().c_str());
        } while (result->NextRow());
        delete result;
    }
}

void InstanceSaveManager::CleanupInstances()
{
    barGoLink bar(2);
    bar.step();

    // load reset times and clean expired instances
    m_Scheduler.LoadResetTimes();

    CharacterDatabase.BeginTransaction();
    // clean character/group - instance binds with invalid group/characters
    _DelHelper(CharacterDatabase, "character_instance.guid, instance", "character_instance", "LEFT JOIN characters ON character_instance.guid = characters.guid WHERE characters.guid IS NULL");
    _DelHelper(CharacterDatabase, "group_instance.leaderGuid, instance", "group_instance", "LEFT JOIN characters ON group_instance.leaderGuid = characters.guid LEFT JOIN groups ON group_instance.leaderGuid = groups.leaderGuid WHERE characters.guid IS NULL OR groups.leaderGuid IS NULL");

    // clean instances that do not have any players or groups bound to them
    _DelHelper(CharacterDatabase, "id, map, difficulty", "instance", "LEFT JOIN character_instance ON character_instance.instance = id LEFT JOIN group_instance ON group_instance.instance = id WHERE character_instance.instance IS NULL AND group_instance.instance IS NULL");

    // clean invalid instance references in other tables
    _DelHelper(CharacterDatabase, "character_instance.guid, instance", "character_instance", "LEFT JOIN instance ON character_instance.instance = instance.id WHERE instance.id IS NULL");
    _DelHelper(CharacterDatabase, "group_instance.leaderGuid, instance", "group_instance", "LEFT JOIN instance ON group_instance.instance = instance.id WHERE instance.id IS NULL");

    // clean unused respawn data
    CharacterDatabase.Execute("DELETE FROM creature_respawn WHERE instance <> 0 AND instance NOT IN (SELECT id FROM instance)");
    CharacterDatabase.Execute("DELETE FROM gameobject_respawn WHERE instance <> 0 AND instance NOT IN (SELECT id FROM instance)");
    //execute transaction directly
    CharacterDatabase.CommitTransaction();

    bar.step();
    sLog.outString();
    sLog.outString( ">> Instances cleaned up");
}

void InstanceSaveManager::PackInstances()
{
    // this routine renumbers player instance associations in such a way so they start from 1 and go up
    // TODO: this can be done a LOT more efficiently

    // obtain set of all associations
    std::set<uint32> InstanceSet;

    // all valid ids are in the instance table
    // any associations to ids not in this table are assumed to be
    // cleaned already in CleanupInstances
    QueryResult *result = CharacterDatabase.Query("SELECT id FROM instance");
    if( result )
    {
        do
        {
            Field *fields = result->Fetch();
            InstanceSet.insert(fields[0].GetUInt32());
        }
        while (result->NextRow());
        delete result;
    }

    barGoLink bar( InstanceSet.size() + 1);
    bar.step();

    uint32 InstanceNumber = 1;
    // we do assume std::set is sorted properly on integer value
    for (std::set<uint32>::iterator i = InstanceSet.begin(); i != InstanceSet.end(); ++i)
    {
        if (*i != InstanceNumber)
        {
            CharacterDatabase.BeginTransaction();
            // remap instance id
            CharacterDatabase.PExecute("UPDATE creature_respawn SET instance = '%u' WHERE instance = '%u'", InstanceNumber, *i);
            CharacterDatabase.PExecute("UPDATE gameobject_respawn SET instance = '%u' WHERE instance = '%u'", InstanceNumber, *i);
            CharacterDatabase.PExecute("UPDATE corpse SET instance = '%u' WHERE instance = '%u'", InstanceNumber, *i);
            CharacterDatabase.PExecute("UPDATE character_instance SET instance = '%u' WHERE instance = '%u'", InstanceNumber, *i);
            CharacterDatabase.PExecute("UPDATE instance SET id = '%u' WHERE id = '%u'", InstanceNumber, *i);
            CharacterDatabase.PExecute("UPDATE group_instance SET instance = '%u' WHERE instance = '%u'", InstanceNumber, *i);
            //execute transaction synchronously
            CharacterDatabase.CommitTransaction();
        }

        ++InstanceNumber;
        bar.step();
    }

    sLog.outString( ">> Instance numbers remapped, next instance id is %u", InstanceNumber );
    sLog.outString();
}

void InstanceSaveManager::_ResetSave(InstanceSaveHashMap& holder, InstanceSaveHashMap::iterator &itr)
{
    // unbind all players bound to the instance
    // do not allow UnbindInstance to automatically unload the InstanceSaves
    lock_instLists = true;
    delete itr->second;
    holder.erase(itr++);
    lock_instLists = false;
}

void InstanceSaveManager::_ResetInstance(uint32 mapid, uint32 instanceId)
{
    DEBUG_LOG("InstanceSaveMgr::_ResetInstance %u, %u", mapid, instanceId);
    Map * iMap = sMapMgr.FindMap(mapid, instanceId);
    if (!iMap || !iMap->Instanceable())
        return;

    InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.find(instanceId);
    if (itr != m_instanceSaveByInstanceId.end())
        _ResetSave(m_instanceSaveByInstanceId, itr);

    DeleteInstanceFromDB(instanceId);                       // even if save not loaded

    if (iMap->IsDungeon())
        ((InstanceMap*)iMap)->Reset(INSTANCE_RESET_RESPAWN_DELAY);
}

void InstanceSaveManager::_ResetOrWarnAll(uint32 mapid, Difficulty difficulty, bool warn, uint32 timeLeft)
{
    // global reset for all instances of the given map
    MapEntry const *mapEntry = sMapStore.LookupEntry(mapid);
    if (!mapEntry->Instanceable())
        return;

    time_t now = time(NULL);

    if (!warn)
    {
        MapDifficulty const* mapDiff = GetMapDifficultyData(mapid,difficulty);
        if (!mapDiff || !mapDiff->resetTime)
        {
            sLog.outError("InstanceSaveManager::ResetOrWarnAll: not valid difficulty or no reset delay for map %d", mapid);
            return;
        }

        // remove all binds to instances of the given map
        for(InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.begin(); itr != m_instanceSaveByInstanceId.end();)
        {
            if (itr->second->GetMapId() == mapid && itr->second->GetDifficulty() == difficulty)
                _ResetSave(m_instanceSaveByInstanceId, itr);
            else
                ++itr;
        }

        // delete them from the DB, even if not loaded
        CharacterDatabase.BeginTransaction();
        CharacterDatabase.PExecute("DELETE FROM character_instance USING character_instance LEFT JOIN instance ON character_instance.instance = id WHERE map = '%u'", mapid);
        CharacterDatabase.PExecute("DELETE FROM group_instance USING group_instance LEFT JOIN instance ON group_instance.instance = id WHERE map = '%u'", mapid);
        CharacterDatabase.PExecute("DELETE FROM instance WHERE map = '%u'", mapid);
        CharacterDatabase.CommitTransaction();

        // calculate the next reset time
        time_t next_reset = InstanceResetScheduler::CalculateNextResetTime(mapDiff, now + timeLeft);
        // update it in the DB
        CharacterDatabase.PExecute("UPDATE instance_reset SET resettime = '"UI64FMTD"' WHERE mapid = '%u' AND difficulty = '%u'", (uint64)next_reset, mapid, difficulty);
    }

    // note: this isn't fast but it's meant to be executed very rarely
    const MapManager::MapMapType& maps = sMapMgr.Maps();

    MapManager::MapMapType::const_iterator iter_last = maps.lower_bound(MapID(mapid + 1));
    for(MapManager::MapMapType::const_iterator mitr = maps.lower_bound(MapID(mapid)); mitr != iter_last; ++mitr)
    {
        Map *map2 = mitr->second;
        if(map2->GetId() != mapid)
            break;

        if (warn)
            ((InstanceMap*)map2)->SendResetWarnings(timeLeft);
        else
            ((InstanceMap*)map2)->Reset(INSTANCE_RESET_GLOBAL);
    }

    // TODO: delete creature/gameobject respawn times even if the maps are not loaded
}

uint32 InstanceSaveManager::GetNumBoundPlayersTotal()
{
    uint32 ret = 0;
    // only instanceable maps have bounds
    for(InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.begin(); itr != m_instanceSaveByInstanceId.end(); ++itr)
        ret += itr->second->GetPlayerCount();
    return ret;
}

uint32 InstanceSaveManager::GetNumBoundGroupsTotal()
{
    uint32 ret = 0;
    // only instanceable maps have bounds
    for(InstanceSaveHashMap::iterator itr = m_instanceSaveByInstanceId.begin(); itr != m_instanceSaveByInstanceId.end(); ++itr)
        ret += itr->second->GetGroupCount();
    return ret;
}

void InstanceSaveManager::_CleanupExpiredInstancesAtTime( time_t t )
{
    _DelHelper(CharacterDatabase, "id, map, instance.difficulty", "instance", "LEFT JOIN instance_reset ON mapid = map AND instance.difficulty =  instance_reset.difficulty WHERE (instance.resettime < '"UI64FMTD"' AND instance.resettime > '0') OR (NOT instance_reset.resettime IS NULL AND instance_reset.resettime < '"UI64FMTD"')",  (uint64)t, (uint64)t);
}

void InstanceSaveManager::LoadCreatureRespawnTimes()
{
    // remove outdated data
    CharacterDatabase.DirectExecute("DELETE FROM creature_respawn WHERE respawntime <= UNIX_TIMESTAMP(NOW())");

    uint32 count = 0;

    QueryResult *result = CharacterDatabase.Query("SELECT guid, respawntime, instance FROM creature_respawn");

    if(!result)
    {
        barGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded 0 creature respawn time.");
        return;
    }

    barGoLink bar((int)result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();
        bar.step();

        uint32 loguid       = fields[0].GetUInt32();
        uint64 respawn_time = fields[1].GetUInt64();
        uint32 instanceId   = fields[2].GetUInt32();

        CreatureData const* data = sObjectMgr.GetCreatureData(loguid);
        if (!data)
            continue;

        MapEntry const* mapEntry = sMapStore.LookupEntry(data->mapid);
        if (!mapEntry || (mapEntry->Instanceable() != (instanceId != 0)))
            continue;

        // instances loaded early and respawn data must exist only for existed instances (save loaded) or non-instanced maps
        InstanceSave* save = instanceId
            ? GetInstanceSave(data->mapid, instanceId)
            : AddInstanceSave(data->mapid, 0, REGULAR_DIFFICULTY, 0, false, true);

        if (!save)
            continue;

        save->SetCreatureRespawnTime(loguid, time_t(respawn_time));

        ++count;

    } while (result->NextRow());

    delete result;

    sLog.outString(">> Loaded %u creature respawn times", count);
    sLog.outString();
}

void InstanceSaveManager::LoadGameobjectRespawnTimes()
{
    // remove outdated data
    CharacterDatabase.DirectExecute("DELETE FROM gameobject_respawn WHERE respawntime <= UNIX_TIMESTAMP(NOW())");

    uint32 count = 0;

    QueryResult *result = CharacterDatabase.Query("SELECT guid, respawntime, instance FROM gameobject_respawn");

    if(!result)
    {
        barGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded 0 gameobject respawn time.");
        return;
    }

    barGoLink bar((int)result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();
        bar.step();

        uint32 loguid       = fields[0].GetUInt32();
        uint64 respawn_time = fields[1].GetUInt64();
        uint32 instanceId   = fields[2].GetUInt32();


        GameObjectData const* data = sObjectMgr.GetGOData(loguid);
        if (!data)
            continue;

        MapEntry const* mapEntry = sMapStore.LookupEntry(data->mapid);
        if (!mapEntry || (mapEntry->Instanceable() != (instanceId != 0)))
            continue;

        // instances loaded early and respawn data must exist only for existed instances (save loaded) or non-instanced maps
        InstanceSave* save = instanceId
            ? GetInstanceSave(data->mapid, instanceId)
            : AddInstanceSave(data->mapid, 0, REGULAR_DIFFICULTY, 0, false, true);

        if (!save)
            continue;

        save->SetGORespawnTime(loguid, time_t(respawn_time));

        ++count;

    } while (result->NextRow());

    delete result;

    sLog.outString(">> Loaded %u gameobject respawn times", count);
    sLog.outString();
}

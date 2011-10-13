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

#include "MapUpdater.h"
#include "DelayExecutor.h"
#include "Map.h"
#include "MapManager.h"
#include "World.h"
#include "Database/DatabaseEnv.h"

#include <ace/Guard_T.h>
#include <ace/Method_Request.h>

class WDBThreadStartReq1 : public ACE_Method_Request
{
    public:

        WDBThreadStartReq1()
        {
        }

        virtual int call()
        {
            WorldDatabase.ThreadStart();
            return 0;
        }
};

class WDBThreadEndReq1 : public ACE_Method_Request
{
    public:

        WDBThreadEndReq1()
        {
        }

        virtual int call()
        {
            WorldDatabase.ThreadEnd();
            return 0;
        }
};

class MapUpdateRequest : public ACE_Method_Request
{
    private:

        Map& m_map;
        MapUpdater& m_updater;
        ACE_UINT32 m_diff;

    public:

        MapUpdateRequest(Map& m, MapUpdater& u, ACE_UINT32 d)
            : m_map(m), m_updater(u), m_diff(d)
        {
        }

        virtual int call()
        {
            ACE_thread_t const threadId = ACE_OS::thr_self();
            m_updater.register_thread(threadId, m_map.GetId(),m_map.GetInstanceId());
            if (m_map.IsBroken())
            {
                m_map.ForcedUnload();
            }
            else
            {
                if (sWorld.getConfig(CONFIG_BOOL_VMSS_STATISTIC_ENABLE))
                    m_map.SetProcessingTime(true);

                m_map.Update(m_diff);

                if (sWorld.getConfig(CONFIG_BOOL_VMSS_STATISTIC_ENABLE))
                    m_map.SetProcessingTime(false);
            }
            m_updater.unregister_thread(threadId);
            m_updater.update_finished ();
            return 0;
        }
};

MapUpdater::MapUpdater()
    : m_mutex(), m_condition(m_mutex), m_executor(), pending_requests(0), m_broken(false)
{
}

MapUpdater::~MapUpdater()
{
    deactivate();
}

int MapUpdater::activate(size_t num_threads)
{
    return m_executor.activate((int)num_threads, new WDBThreadStartReq1, new WDBThreadEndReq1);
}

int MapUpdater::deactivate()
{
    wait();

    return m_executor.deactivate();
}

void MapUpdater::ReActivate(uint32 threads)
{
    deactivate();
    activate(threads);
    SetBroken(false);
}

int MapUpdater::wait()
{
    ACE_GUARD_RETURN(ACE_Thread_Mutex, guard, m_mutex, -1);

    while (pending_requests > 0)
        m_condition.wait();

    return 0;
}

int MapUpdater::schedule_update(Map& map, ACE_UINT32 diff)
{
    ACE_GUARD_RETURN(ACE_Thread_Mutex, guard, m_mutex, -1);

    ++pending_requests;

    if (m_executor.execute(new MapUpdateRequest(map, *this, diff)) == -1)
    {
        ACE_DEBUG((LM_ERROR, ACE_TEXT("(%t) \n"), ACE_TEXT("Failed to schedule Map Update")));

        --pending_requests;
        return -1;
    }

    return 0;
}

bool MapUpdater::activated()
{
    return m_executor.activated();
}

void MapUpdater::update_finished()
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_mutex);

    if (pending_requests == 0)
    {
        ACE_ERROR((LM_ERROR, ACE_TEXT("(%t)\n"), ACE_TEXT("MapUpdater::update_finished BUG, report to devs")));
        return;
    }

    --pending_requests;

    m_condition.broadcast();
}

void MapUpdater::register_thread(ACE_thread_t const threadId, uint32 mapId, uint32 instanceId)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_mutex);
    MapID pair = MapID(mapId, instanceId);
    m_threads.insert(std::make_pair(threadId, pair));
    m_starttime.insert(std::make_pair(threadId, WorldTimer::getMSTime()));
}

void MapUpdater::unregister_thread(ACE_thread_t const threadId)
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_mutex);
    m_threads.erase(threadId);
    m_starttime.erase(threadId);
}

MapID const* MapUpdater::GetMapPairByThreadId(ACE_thread_t const threadId)
{
    if (!m_threads.empty())
    {
        ThreadMapMap::const_iterator itr = m_threads.find(threadId);
        if (itr != m_threads.end())
            return &itr->second;
    }
    return NULL;
}

void MapUpdater::FreezeDetect()
{
    ACE_GUARD(ACE_Thread_Mutex, guard, m_mutex);
    if (!m_starttime.empty())
    {
        for (ThreadStartTimeMap::const_iterator itr = m_starttime.begin(); itr != m_starttime.end(); ++itr)
        if (WorldTimer::getMSTime() - itr->second > sWorld.getConfig(CONFIG_UINT32_VMSS_FREEZEDETECTTIME))
        {
            if (MapID const* mapPair = GetMapPairByThreadId(itr->first))
            {
                DEBUG_LOG("MapUpdater::FreezeDetect thread "I64FMT" possible freezed (is update map %u instance %u). Killing.",itr->first,mapPair->nMapId, mapPair->nInstanceId);
                ACE_OS::thr_kill(itr->first, SIGUSR1);
            }
        }
    }
}

void MapUpdater::MapBrokenEvent(MapID const* mapPair)
{
    if (!m_brokendata.empty())
    {
        MapBrokenDataMap::iterator itr =  m_brokendata.find(*mapPair);
        if (itr != m_brokendata.end())
        {
            if ((time(NULL) - itr->second.lastErrorTime) > sWorld.getConfig(CONFIG_UINT32_VMSS_TBREMTIME))
                itr->second.Reset();
            else
                itr->second.IncreaseCount();
            return;
        }
    }
    m_brokendata.insert(std::make_pair(*mapPair, MapBrokenData()));
}

MapBrokenData const* MapUpdater::GetMapBrokenData(MapID const* mapPair)
{
    if (!m_brokendata.empty())
    {
        MapBrokenDataMap::const_iterator itr =  m_brokendata.find(*mapPair);
        if (itr != m_brokendata.end())
            return &itr->second;
    }
    return NULL;
}

void Map::SetProcessingTime(bool stage)
{
    if (stage)
    {
        m_lastStartTime = WorldTimer::getMSTime();
        ++m_updatesCount;
    }
    else if (m_updatesCount)
    {
        uint32 timeDiff = WorldTimer::getMSTimeDiff(WorldTimer::getMSTime(), m_lastStartTime);
        m_executionTime += timeDiff;
    }
}

void Map::ResetStatistic(bool full)
{
    if (full)
    {
        m_lastStartTime = 0;
        m_updatesCount = 0;
        m_executionTime = 0;
    }

    for (size_t i = 0; i != MAX_TYPE_ID; ++i)
    {
        switch (TypeID(i))
        {
            case TYPEID_CORPSE:
                if (full)
                    m_objectCount[i] = 0;
                break;
            default:
                m_objectCount[i] = 0;
                break;
        }
    }
}

void Map::AddProcessedObject(uint8 typeId, bool type)
{
    if (!sWorld.getConfig(CONFIG_BOOL_VMSS_STATISTIC_ENABLE))
        return;
    if (type)
        ++m_objectCount[typeId];
    else if (m_objectCount[typeId] > 0)
        ++m_objectCount[typeId];
}

void Map::PrintStatistic()
{
    sLog.outDetail("Map::Statistic map (id %u, inst %u, diff %u): AVG exec time %f ms, objects/type: %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u",
    GetId(), GetInstanceId(), GetDifficulty(),
    ((float)m_executionTime/(float)m_updatesCount),
    m_objectCount[TYPEID_OBJECT],TYPEID_OBJECT,
    m_objectCount[TYPEID_ITEM],TYPEID_ITEM,
    m_objectCount[TYPEID_CONTAINER],TYPEID_CONTAINER,
    m_objectCount[TYPEID_UNIT],TYPEID_UNIT,
    m_objectCount[TYPEID_PLAYER],TYPEID_PLAYER,
    m_objectCount[TYPEID_GAMEOBJECT],TYPEID_GAMEOBJECT,
    m_objectCount[TYPEID_DYNAMICOBJECT],TYPEID_DYNAMICOBJECT,
    m_objectCount[TYPEID_CORPSE],TYPEID_CORPSE
    );

}

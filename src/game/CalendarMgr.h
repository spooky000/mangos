
#ifndef MANGOS_CALENDARMGR_H
#define MANGOS_CALENDARMGR_H

#include <ace/Singleton.h>
#include "Calendar.h"
#include "ObjectGuid.h"

class CalendarMgr
{
        friend class ACE_Singleton<CalendarMgr, ACE_Null_Mutex>;

        CalendarMgr();
        ~CalendarMgr();

    public:
        void LoadFromDB();

        CalendarInvite* GetInvite(uint32 inviteId);
        CalendarEvent* GetEvent(uint32 eventId);

        CalendarInviteIdList const& GetPlayerInvites(ObjectGuid guid);
        CalendarEventIdList const& GetPlayerEvents(ObjectGuid guid);

        uint32 GetPlayerNumPending(ObjectGuid guid);
        uint32 GetFreeEventId();
        uint32 GetFreeInviteId();

        void AddAction(CalendarAction const& action);

        void SendCalendarEvent(CalendarEvent const& calendarEvent, CalendarSendEventType type);
        void SendCalendarEventInvite(CalendarInvite const& invite, bool pending);
        void SendCalendarEventInviteAlert(CalendarEvent const& calendarEvent, CalendarInvite const& invite);
        void SendCalendarEventInviteRemove(ObjectGuid guid, CalendarInvite const& invite, uint32 flags);
        void SendCalendarEventInviteRemoveAlert(ObjectGuid guid, CalendarEvent const& calendarEvent, CalendarInviteStatus status);
        void SendCalendarEventUpdateAlert(ObjectGuid guid, CalendarEvent const& calendarEvent, CalendarSendEventType type);
        void SendCalendarEventStatus(ObjectGuid guid, CalendarEvent const& calendarEvent, CalendarInvite const& invite);
        void SendCalendarEventRemovedAlert(ObjectGuid guid, CalendarEvent const& calendarEvent);
        void SendCalendarEventModeratorStatusAlert(CalendarInvite const& invite);

    private:
        CalendarEvent* CheckPermisions(uint32 eventId, Player* player, uint32 inviteId, CalendarModerationRank minRank);

        bool AddEvent(CalendarEvent const& calendarEvent);
        bool RemoveEvent(uint32 eventId);
        bool AddPlayerEvent(ObjectGuid guid, uint32 eventId);
        bool RemovePlayerEvent(ObjectGuid guid, uint32 eventId);

        bool AddInvite(CalendarInvite const& invite);
        uint32 RemoveInvite(uint32 inviteId);
        bool AddPlayerInvite(ObjectGuid guid, uint32 inviteId);
        bool RemovePlayerInvite(ObjectGuid guid, uint32 inviteId);

        CalendarEventMap _events;
        CalendarInviteMap _invites;
        CalendarPlayerInviteIdMap _playerInvites;
        CalendarPlayerEventIdMap _playerEvents;

        uint64 _eventNum;
        uint64 _inviteNum;
};

#define sCalendarMgr ACE_Singleton<CalendarMgr, ACE_Null_Mutex>::instance()

#endif

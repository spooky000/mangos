
#ifndef MANGOS_CALENDARMGR_H
#define MANGOS_CALENDARMGR_H

#include <ace/Singleton.h>
#include "Calendar.h"

class CalendarMgr
{
        friend class ACE_Singleton<CalendarMgr, ACE_Null_Mutex>;

        CalendarMgr();
        ~CalendarMgr();

    public:
        void LoadFromDB();

        CalendarInvite* GetInvite(uint64 inviteId);
        CalendarEvent* GetEvent(uint64 eventId);

        CalendarInviteIdList const& GetPlayerInvites(ObjectGuid guid);
        CalendarEventIdList const& GetPlayerEvents(ObjectGuid guid);

        uint32 GetPlayerNumPending(ObjectGuid guid);
        uint64 GetFreeEventId();
        uint64 GetFreeInviteId();

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
        CalendarEvent* CheckPermisions(uint64 eventId, Player* player, uint64 inviteId, CalendarModerationRank minRank);

        bool AddEvent(CalendarEvent const& calendarEvent);
        bool RemoveEvent(uint64 eventId);
        bool AddPlayerEvent(ObjectGuid guid, uint64 eventId);
        bool RemovePlayerEvent(ObjectGuid guid, uint64 eventId);

        bool AddInvite(CalendarInvite const& invite);
        uint64 RemoveInvite(uint64 inviteId);
        bool AddPlayerInvite(ObjectGuid guid, uint64 inviteId);
        bool RemovePlayerInvite(ObjectGuid guid, uint64 inviteId);

        CalendarEventMap _events;
        CalendarInviteMap _invites;
        CalendarPlayerInviteIdMap _playerInvites;
        CalendarPlayerEventIdMap _playerEvents;

        uint64 _eventNum;
        uint64 _inviteNum;
};

#define sCalendarMgr ACE_Singleton<CalendarMgr, ACE_Null_Mutex>::instance()

#endif

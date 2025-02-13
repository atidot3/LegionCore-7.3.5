/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "ObjectDefines.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "SpellMgr.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "CellImpl.h"
#include "InstanceScript.h"
#include "ScriptedCreature.h"
#include "GameEventMgr.h"
#include "CreatureTextMgr.h"
#include "DB2Stores.h"
#include "SmartScriptMgr.h"
#include "EventObjectData.h"
#include "QuestData.h"
#include "StringConvert.h"

void SmartWaypointMgr::LoadFromDB()
{
    uint32 oldMSTime = getMSTime();

    for (std::unordered_map<uint32, WPPath*>::iterator itr = waypoint_map.begin(); itr != waypoint_map.end(); ++itr)
    {
        for (WPPath::iterator pathItr = itr->second->begin(); pathItr != itr->second->end(); ++pathItr)
            delete pathItr->second;

        delete itr->second;
    }

    waypoint_map.clear();

    WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_SMARTAI_WP);
    PreparedQueryResult result = WorldDatabase.Query(stmt);

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 SmartAI Waypoint Paths. DB table `waypoints` is empty.");

        return;
    }

    uint32 count = 0;
    uint32 total = 0;
    uint32 last_entry = 0;
    uint32 last_id = 1;

    do
    {
        Field* fields = result->Fetch();
        uint32 entry = fields[0].GetUInt32();
        uint32 id = fields[1].GetUInt32();
        float x = fields[2].GetFloat();
        float y = fields[3].GetFloat();
        float z = fields[4].GetFloat();

        if (last_entry != entry)
        {
            waypoint_map[entry] = new WPPath();
            last_id = 1;
            count++;
        }

        if (last_id != id)
            TC_LOG_ERROR("sql.sql", "SmartWaypointMgr::LoadFromDB: Path entry %u, unexpected point id %u, expected %u.", entry, id, last_id);

        last_id++;
        (*waypoint_map[entry])[id] = new WayPoint(id, x, y, z);

        last_entry = entry;
        total++;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded %u SmartAI waypoint paths (total %u waypoints) in %u ms", count, total, GetMSTimeDiffToNow(oldMSTime));

}

WPPath* SmartWaypointMgr::GetPath(uint32 id)
{
    if (waypoint_map.find(id) != waypoint_map.end())
        return waypoint_map[id];
    return nullptr;
}

WayPoint::WayPoint(uint32 _id, float _x, float _y, float _z)
{
    id = _id;
    x = _x;
    y = _y;
    z = _z;
}

SmartTarget::SmartTarget(SMARTAI_TARGETS t, uint32 p1, uint32 p2, uint32 p3, uint32 p4): x(0), y(0), z(0), o(0)
{
    type = t;
    raw.param1 = p1;
    raw.param2 = p2;
    raw.param3 = p3;
    raw.param3 = p4;
}

SmartScriptHolder::SmartScriptHolder(): entryOrGuid(0), source_type(SMART_SCRIPT_TYPE_CREATURE), event_id(0), link(0), event{}, action{}, timer(0), active(false), runOnce(false), enableTimed(false)
{
}

SmartWaypointMgr::~SmartWaypointMgr()
{
    for (std::unordered_map<uint32, WPPath*>::iterator itr = waypoint_map.begin(); itr != waypoint_map.end(); ++itr)
    {
        for (WPPath::iterator pathItr = itr->second->begin(); pathItr != itr->second->end(); ++pathItr)
            delete pathItr->second;

        delete itr->second;
    }

    waypoint_map.clear();
}

SmartWaypointMgr* SmartWaypointMgr::instance()
{
    static SmartWaypointMgr instance;
    return &instance;
}

SmartAIMgr* SmartAIMgr::instance()
{
    static SmartAIMgr instance;
    return &instance;
}

void SmartAIMgr::LoadSmartAIFromDB()
{
    uint32 oldMSTime = getMSTime();

    for (uint8 i = 0; i < SMART_SCRIPT_TYPE_MAX; i++)
        mEventMap[i].clear();  //Drop Existing SmartAI List

    WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_SMART_SCRIPTS);
    PreparedQueryResult result = WorldDatabase.Query(stmt);

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 SmartAI scripts. DB table `smartai_scripts` is empty.");

        return;
    }

    uint32 count = 0;

    do
    {
        Field* fields = result->Fetch();

        SmartScriptHolder temp;

        temp.entryOrGuid = fields[0].GetInt64();
        SmartScriptType source_type = (SmartScriptType)fields[1].GetUInt8();
        if (source_type >= SMART_SCRIPT_TYPE_MAX)
        {
            TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: invalid source_type (%u), skipped loading.", uint32(source_type));
            continue;
        }
        if (temp.entryOrGuid >= 0)
        {
            switch (source_type)
            {
                case SMART_SCRIPT_TYPE_CREATURE:
                {
                    if (!sObjectMgr->GetCreatureTemplate((uint32)temp.entryOrGuid))
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: Creature entry (%u) does not exist, skipped loading.", uint32(temp.entryOrGuid));
                        continue;
                    }
                    break;
                }
                case SMART_SCRIPT_TYPE_GAMEOBJECT:
                {
                    if (!sObjectMgr->GetGameObjectTemplate((uint32)temp.entryOrGuid))
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: GameObject entry (%u) does not exist, skipped loading.", uint32(temp.entryOrGuid));
                        continue;
                    }
                    break;
                }
                case SMART_SCRIPT_TYPE_AREATRIGGER:
                {
                    if (!sAreaTriggerStore.LookupEntry((uint32)temp.entryOrGuid))
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: AreaTrigger entry (%u) does not exist, skipped loading.", uint32(temp.entryOrGuid));
                        continue;
                    }
                    break;
                }
                case SMART_SCRIPT_TYPE_EVENTOBJECT:
                {
                    if (!sEventObjectDataStore->GetEventObjectTemplate((uint32)temp.entryOrGuid))
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: EventObject entry (%u) does not exist, skipped loading.", uint32(temp.entryOrGuid));
                        continue;
                    }
                    break;
                }
                case SMART_SCRIPT_TYPE_TIMED_ACTIONLIST:
                    break;//nothing to check, really
                default:
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: not yet implemented source_type %u", (uint32)source_type);
                    continue;
            }
        }
        else
        {
            if (!sObjectMgr->GetCreatureData(uint32(abs(temp.entryOrGuid))))
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: Creature guid (%u) does not exist, skipped loading.", uint32(abs(temp.entryOrGuid)));
                continue;
            }
        }

        temp.source_type = source_type;
        temp.event_id = fields[2].GetUInt16();
        temp.link = fields[3].GetUInt16();

        bool invalidDifficulties = false;
        for (std::string_view token : Trinity::Tokenize(fields[4].GetStringView(), ',', false))
        {
            Optional<std::underlying_type_t<Difficulty>> tokenValue = Trinity::StringTo<std::underlying_type_t<Difficulty>>(token);
            if (!tokenValue.has_value())
            {
                invalidDifficulties = true;
                TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: Invalid difficulties for entryorguid (%ld) source_type (%u) id (%u), skipped loading.",
                             temp.entryOrGuid, temp.GetScriptType(), temp.event_id);
                break;
            }

            Difficulty difficultyId = Difficulty(tokenValue.value());
            if (difficultyId && !sDifficultyStore.LookupEntry(difficultyId))
            {
                invalidDifficulties = true;
                TC_LOG_ERROR("sql.sql", "SmartAIMgr::LoadSmartAIFromDB: Invalid difficulty id (%u) for entryorguid (%ld) source_type (%u) id (%u), skipped loading.",
                             difficultyId, temp.entryOrGuid, temp.GetScriptType(), temp.event_id);
                break;
            }

            temp.Difficulties.push_back(difficultyId);
        }

        if (invalidDifficulties)
            continue;

        temp.event.type = (SMART_EVENT)fields[5].GetUInt8();
        temp.event.event_phase_mask = fields[6].GetUInt8();
        temp.event.event_chance = fields[7].GetUInt8();
        temp.event.event_flags = fields[8].GetUInt32();

        temp.event.raw.param1 = fields[9].GetUInt32();
        temp.event.raw.param2 = fields[10].GetUInt32();
        temp.event.raw.param3 = fields[11].GetUInt32();
        temp.event.raw.param4 = fields[12].GetUInt32();
        temp.event.raw.param5 = fields[13].GetUInt32();

        temp.action.type = (SMART_ACTION)fields[14].GetUInt8();
        temp.action.raw.param1 = fields[15].GetUInt32();
        temp.action.raw.param2 = fields[16].GetUInt32();
        temp.action.raw.param3 = fields[17].GetUInt32();
        temp.action.raw.param4 = fields[18].GetUInt32();
        temp.action.raw.param5 = fields[19].GetUInt32();
        temp.action.raw.param6 = fields[20].GetUInt32();

        temp.target.type = (SMARTAI_TARGETS)fields[21].GetUInt8();
        temp.target.raw.param1 = fields[22].GetUInt32();
        temp.target.raw.param2 = fields[23].GetUInt32();
        temp.target.raw.param3 = fields[24].GetUInt32();
        temp.target.raw.param4 = fields[25].GetUInt32();
        temp.target.x = fields[26].GetFloat();
        temp.target.y = fields[27].GetFloat();
        temp.target.z = fields[28].GetFloat();
        temp.target.o = fields[29].GetFloat();

        //check target
        if (!IsTargetValid(temp))
            continue;

        // check all event and action params
        if (!IsEventValid(temp))
            continue;

        // creature entry / guid not found in storage, create empty event list for it and increase counters
        if (mEventMap[source_type].find(temp.entryOrGuid) == mEventMap[source_type].end())
        {
            ++count;
            SmartAIEventList eventList;
            mEventMap[source_type][temp.entryOrGuid] = eventList;
        }
        // store the new event
        mEventMap[source_type][temp.entryOrGuid].push_back(temp);
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded %u SmartAI scripts in %u ms", count, GetMSTimeDiffToNow(oldMSTime));

}

SmartAIEventList SmartAIMgr::GetScript(int32 entry, SmartScriptType type)
{
    SmartAIEventList temp;
    uint32 _type = type;
    if (mEventMap[_type].find(entry) != mEventMap[_type].end())
        return mEventMap[_type][entry];
    if (entry > 0)//first search is for guid (negative), do not drop error if not found
    TC_LOG_DEBUG("scripts.ai", "SmartAIMgr::GetScript: Could not load Script for Entry %d ScriptType %u.", entry, uint32(type));
    return temp;
}

bool SmartAIMgr::IsTargetValid(SmartScriptHolder const& e)
{
    if (e.GetActionType() == SMART_ACTION_INSTALL_AI_TEMPLATE)
        return true; // AI template has special handling
    switch (e.GetTargetType())
    {
        case SMART_TARGET_CREATURE_DISTANCE:
        case SMART_TARGET_CREATURE_RANGE:
        {
            if (e.target.unitDistance.creature && !sObjectMgr->GetCreatureTemplate(e.target.unitDistance.creature))
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Creature entry %u as target_param1, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.target.unitDistance.creature);
                return false;
            }
            break;
        }
        case SMART_TARGET_GAMEOBJECT_DISTANCE:
        case SMART_TARGET_GAMEOBJECT_RANGE:
        {
            if (e.target.goDistance.entry && !sObjectMgr->GetGameObjectTemplate(e.target.goDistance.entry))
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent GameObject entry %u as target_param1, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.target.goDistance.entry);
                return false;
            }
            break;
        }
        case SMART_TARGET_CREATURE_GUID:
        {
            if (e.target.unitGUID.entry && !IsCreatureValid(e, e.target.unitGUID.entry))
                return false;
            break;
        }
        case SMART_TARGET_GAMEOBJECT_GUID:
        {
            if (e.target.goGUID.entry && !IsGameObjectValid(e, e.target.goGUID.entry))
                return false;
            break;
        }
        case SMART_TARGET_PLAYER_DISTANCE:
        case SMART_TARGET_CLOSEST_PLAYER:
        {
            if (e.target.playerDistance.dist == 0)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u has maxDist 0 as target_param1, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                return false;
            }
            break;
        }
        case SMART_TARGET_PLAYER_RANGE:
        case SMART_TARGET_SELF:
        case SMART_TARGET_VICTIM:
        case SMART_TARGET_HOSTILE_SECOND_AGGRO:
        case SMART_TARGET_HOSTILE_LAST_AGGRO:
        case SMART_TARGET_HOSTILE_RANDOM:
        case SMART_TARGET_HOSTILE_RANDOM_NOT_TOP:
        case SMART_TARGET_HOSTILE_RANDOM_PLAYER:
        case SMART_TARGET_HOSTILE_RANDOM_NOT_TOP_PLAYER:
        case SMART_TARGET_HOSTILE_RANDOM_AURA:
        case SMART_TARGET_ACTION_INVOKER:
        case SMART_TARGET_INVOKER_PARTY:
        case SMART_TARGET_POSITION:
        case SMART_TARGET_NONE:
        case SMART_TARGET_ACTION_INVOKER_VEHICLE:
        case SMART_TARGET_OWNER_OR_SUMMONER:
        case SMART_TARGET_THREAT_LIST:
        case SMART_TARGET_CLOSEST_GAMEOBJECT:
        case SMART_TARGET_CLOSEST_CREATURE:
        case SMART_TARGET_CLOSEST_ENEMY:
        case SMART_TARGET_CLOSEST_FRIENDLY:
        case SMART_TARGET_STORED:
        case SMART_TARGET_RANDOM_POSITION:
        case SMART_EVENT_QUEST_REWARDED:
        case SMART_EVENT_QUEST_ACCEPTED:
        case SMART_TARGET_TARGETUNIT:
            break;
        default:
            TC_LOG_ERROR("sql.sql", "SmartAIMgr: Not handled target_type(%u), Entry %ld SourceType %u Event %u Action %u, skipped.", e.GetTargetType(), e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
            return false;
    }
    return true;
}

bool SmartAIMgr::IsMinMaxValid(SmartScriptHolder const& e, uint32 min, uint32 max)
{
    if (max < min)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses min/max params wrong (%u/%u), skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), min, max);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsEventValid(SmartScriptHolder& e)
{
    if (e.event.type >= SMART_EVENT_END)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld using event (%u) has invalid event type (%u), skipped.", e.entryOrGuid, e.event_id, e.GetEventType());
        return false;
    }

    // in SMART_SCRIPT_TYPE_TIMED_ACTIONLIST all event types are overriden by core
    if (e.GetScriptType() != SMART_SCRIPT_TYPE_TIMED_ACTIONLIST && !(SmartAIEventMask[e.event.type][1] & SmartAITypeMask[e.GetScriptType()][1]))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld, event type %u can not be used for Script type %u", e.entryOrGuid, e.GetEventType(), e.GetScriptType());
        return false;
    }

    if (e.action.type <= 0 || e.action.type >= SMART_ACTION_END)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld using event (%u) has invalid action type (%u), skipped.", e.entryOrGuid, e.event_id, e.GetActionType());
        return false;
    }

    if (e.event.event_phase_mask > SMART_EVENT_PHASE_ALL)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld using event (%u) has invalid phase mask (%u), skipped.", e.entryOrGuid, e.event_id, e.event.event_phase_mask);
        return false;
    }

    if (e.event.event_flags > SMART_EVENT_FLAGS_ALL)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld using event (%u) has invalid event flags (%u), skipped.", e.entryOrGuid, e.event_id, e.event.event_flags);
        return false;
    }

    if (e.Difficulties.empty() && e.event.event_flags & SMART_EVENT_FLAGS_DEPRECATED)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld using event (%u) has deprecated event flags (%u), skipped.", e.entryOrGuid, e.event_id, e.event.event_flags);
        return false;
    }

    if (e.link && e.link == e.event_id)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: EntryOrGuid %ld SourceType %u, Event %u, Event is linking self (infinite loop), skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id);
        return false;
    }

    if (e.GetScriptType() == SMART_SCRIPT_TYPE_TIMED_ACTIONLIST)
    {
        e.event.type = SMART_EVENT_UPDATE_OOC;//force default OOC, can change when calling the script!
        if (!IsMinMaxValid(e, e.event.minMaxRepeat.min, e.event.minMaxRepeat.max))
            return false;

        if (!IsMinMaxValid(e, e.event.minMaxRepeat.repeatMin, e.event.minMaxRepeat.repeatMax))
            return false;
    }
    else
    {
        uint32 type = e.event.type;
        switch (type)
        {
            case SMART_EVENT_UPDATE:
            case SMART_EVENT_UPDATE_IC:
            case SMART_EVENT_UPDATE_OOC:
            case SMART_EVENT_HEALTH_PCT:
            case SMART_EVENT_MANA_PCT:
            case SMART_EVENT_TARGET_HEALTH_PCT:
            case SMART_EVENT_TARGET_MANA_PCT:
            case SMART_EVENT_RANGE:
            case SMART_EVENT_DAMAGED:
            case SMART_EVENT_DAMAGED_TARGET:
            case SMART_EVENT_RECEIVE_HEAL:
                if (!IsMinMaxValid(e, e.event.minMaxRepeat.min, e.event.minMaxRepeat.max))
                    return false;

                if (!IsMinMaxValid(e, e.event.minMaxRepeat.repeatMin, e.event.minMaxRepeat.repeatMax))
                    return false;

                // Fix spamm when not set value
                if (e.event.minMaxRepeat.repeatMin < 500)
                    e.event.minMaxRepeat.repeatMin = 500;
                if (e.event.minMaxRepeat.repeatMax < 500)
                    e.event.minMaxRepeat.repeatMax = 500;
                break;
            case SMART_EVENT_SPELLHIT:
            case SMART_EVENT_SPELLHIT_TARGET:
                if (e.event.spellHit.spell)
                {
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(e.event.spellHit.spell);
                    if (!spellInfo)
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Spell entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.spellHit.spell);
                        return false;
                    }
                    if (e.event.spellHit.school && (e.event.spellHit.school & spellInfo->GetMisc()->MiscData.SchoolMask) != spellInfo->GetMisc()->MiscData.SchoolMask)
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses Spell entry %u with invalid school mask, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.spellHit.spell);
                        return false;
                    }
                }
                if (!IsMinMaxValid(e, e.event.spellHit.cooldownMin, e.event.spellHit.cooldownMax))
                    return false;
                break;
            case SMART_EVENT_OOC_LOS:
            case SMART_EVENT_IC_LOS:
                if (!IsMinMaxValid(e, e.event.los.cooldownMin, e.event.los.cooldownMax))
                    return false;
                break;
            case SMART_EVENT_CHECK_DIST_TO_HOME:
                if (!IsMinMaxValid(e, e.event.dist.repeatMin, e.event.dist.repeatMax))
                    return false;
                break;
            case SMART_EVENT_RESPAWN:
                if (e.event.respawn.type == SMART_SCRIPT_RESPAWN_CONDITION_MAP && !sMapStore.LookupEntry(e.event.respawn.map))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Map entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.respawn.map);
                    return false;
                }
                if (e.event.respawn.type == SMART_SCRIPT_RESPAWN_CONDITION_AREA && !sAreaTableStore.LookupEntry(e.event.respawn.area))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Area entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.respawn.area);
                    return false;
                }
                break;
            case SMART_EVENT_FRIENDLY_HEALTH:
                if (!NotNULL(e, e.event.friendlyHealth.radius))
                    return false;

                if (!IsMinMaxValid(e, e.event.friendlyHealth.repeatMin, e.event.friendlyHealth.repeatMax))
                    return false;
                break;
            case SMART_EVENT_FRIENDLY_IS_CC:
                if (!IsMinMaxValid(e, e.event.friendlyCC.repeatMin, e.event.friendlyCC.repeatMax))
                    return false;
                break;
            case SMART_EVENT_FRIENDLY_MISSING_BUFF:
            {
                if (!IsSpellValid(e, e.event.missingBuff.spell))
                    return false;

                if (!NotNULL(e, e.event.missingBuff.radius))
                    return false;

                if (!IsMinMaxValid(e, e.event.missingBuff.repeatMin, e.event.missingBuff.repeatMax))
                    return false;
                break;
            }
            case SMART_EVENT_KILL:
                if (!IsMinMaxValid(e, e.event.kill.cooldownMin, e.event.kill.cooldownMax))
                    return false;

                if (e.event.kill.creature && !IsCreatureValid(e, e.event.kill.creature))
                    return false;
                break;
            case SMART_EVENT_VICTIM_CASTING:
            case SMART_EVENT_PASSENGER_BOARDED:
            case SMART_EVENT_PASSENGER_REMOVED:
                if (!IsMinMaxValid(e, e.event.minMax.repeatMin, e.event.minMax.repeatMax))
                    return false;
                break;
            case SMART_EVENT_SUMMON_DESPAWNED:
            case SMART_EVENT_SUMMONED_UNIT:
                if (e.event.summoned.creature && !IsCreatureValid(e, e.event.summoned.creature))
                    return false;

                if (!IsMinMaxValid(e, e.event.summoned.cooldownMin, e.event.summoned.cooldownMax))
                    return false;
                break;
            case SMART_EVENT_ACCEPTED_QUEST:
            case SMART_EVENT_REWARD_QUEST:
                if (e.event.quest.quest && !IsQuestValid(e, e.event.quest.quest))
                    return false;
                break;
            case SMART_EVENT_RECEIVE_EMOTE:
            {
                if (e.event.emote.emote && !IsTextEmoteValid(e, e.event.emote.emote))
                    return false;

                if (!IsMinMaxValid(e, e.event.emote.cooldownMin, e.event.emote.cooldownMax))
                    return false;
                break;
            }
            case SMART_EVENT_HAS_AURA:
            case SMART_EVENT_TARGET_BUFFED:
            {
                if (!IsSpellValid(e, e.event.aura.spell))
                    return false;

                if (!IsMinMaxValid(e, e.event.aura.repeatMin, e.event.aura.repeatMax))
                    return false;
                break;
            }
            case SMART_EVENT_TRANSPORT_ADDCREATURE:
            {
                if (e.event.transportAddCreature.creature && !IsCreatureValid(e, e.event.transportAddCreature.creature))
                    return false;
                break;
            }
            case SMART_EVENT_MOVEMENTINFORM:
            {
                if (e.event.movementInform.type > NULL_MOTION_TYPE)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses invalid Motion type %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.movementInform.type);
                    return false;
                }
                break;
            }
            case SMART_EVENT_DATA_SET:
            {
                if (!IsMinMaxValid(e, e.event.dataSet.cooldownMin, e.event.dataSet.cooldownMax))
                    return false;
                break;
            }
            case SMART_EVENT_AREATRIGGER_ONTRIGGER:
            {
                if (e.event.areatrigger.id && !IsAreaTriggerValid(e, e.event.areatrigger.id))
                    return false;
                break;
            }
            case SMART_EVENT_TEXT_OVER:
                //if (e.event.textOver.textGroupID && !IsTextValid(e, e.event.textOver.textGroupID)) return false;// 0 is a valid text group!
                break;
            case SMART_EVENT_LINK:
            {
                if (e.link && e.link == e.event_id)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u, Event %u, Link Event is linking self (infinite loop), skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id);
                    return false;
                }
                break;
            }
            case SMART_EVENT_DUMMY_EFFECT:
            {
                if (!IsSpellValid(e, e.event.dummy.spell))
                    return false;

                if (e.event.dummy.effIndex > EFFECT_2)
                    return false;
                break;
            }
            case SMART_EVENT_IS_BEHIND_TARGET:
            {
                if (!IsMinMaxValid(e, e.event.behindTarget.cooldownMin, e.event.behindTarget.cooldownMax))
                    return false;
                break;
            }
            case SMART_EVENT_GAME_EVENT_START:
            case SMART_EVENT_GAME_EVENT_END:
            {
                GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();
                if (e.event.gameEvent.gameEventId >= events.size() || !events[e.event.gameEvent.gameEventId].isValid())
                    return false;
                break;
            }
            case SMART_EVENT_ACTION_DONE:
            {
                if (e.event.doAction.eventId > EVENT_CHARGE)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses invalid event id %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.event.doAction.eventId);
                    return false;
                }
                break;
            }
			case SMART_EVENT_DISTANCE_CREATURE:
				if (e.event.distance.guid == 0 && e.event.distance.entry == 0)
				{
					TC_LOG_ERROR("sql.sql", "SmartAIMgr: Event SMART_EVENT_DISTANCE_CREATURE did not provide creature guid or entry, skipped.");
					return false;
				}

				if (e.event.distance.guid != 0 && e.event.distance.entry != 0)
				{
					TC_LOG_ERROR("sql.sql", "SmartAIMgr: Event SMART_EVENT_DISTANCE_CREATURE provided both an entry and guid, skipped.");
					return false;
				}

				if (e.event.distance.guid != 0 && !sObjectMgr->GetCreatureData(e.event.distance.guid))
				{
					TC_LOG_ERROR("sql.sql", "SmartAIMgr: Event SMART_EVENT_DISTANCE_CREATURE using invalid creature guid %u, skipped.", e.event.distance.guid);
					return false;
				}

				if (e.event.distance.entry != 0 && !sObjectMgr->GetCreatureTemplate(e.event.distance.entry))
				{
					TC_LOG_ERROR("sql.sql", "SmartAIMgr: Event SMART_EVENT_DISTANCE_CREATURE using invalid creature entry %u, skipped.", e.event.distance.entry);
					return false;
				}
				break;
            case SMART_EVENT_FRIENDLY_HEALTH_PCT:
                if (!IsMinMaxValid(e, e.event.friendlyHealthPct.repeatMin, e.event.friendlyHealthPct.repeatMax))
                    return false;
                if (e.event.friendlyHealthPct.maxHpPct > 100 || e.event.friendlyHealthPct.minHpPct > 100)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u has pct value above 100, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                    return false;
                }
                switch (e.GetTargetType())
                {
                    case SMART_TARGET_CREATURE_RANGE:
                    case SMART_TARGET_CREATURE_GUID:
                    case SMART_TARGET_CREATURE_DISTANCE:
                    case SMART_TARGET_CLOSEST_CREATURE:
                    case SMART_TARGET_CLOSEST_PLAYER:
                    case SMART_TARGET_PLAYER_RANGE:
                    case SMART_TARGET_PLAYER_DISTANCE:
                        break;
                    default:
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses invalid target_type %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.GetTargetType());
                        return false;
                }
                break;
            case SMART_EVENT_GO_STATE_CHANGED:
            case SMART_EVENT_GO_EVENT_INFORM:
            case SMART_EVENT_TIMED_EVENT_TRIGGERED:
            case SMART_EVENT_INSTANCE_PLAYER_ENTER:
            case SMART_EVENT_TRANSPORT_RELOCATE:
            case SMART_EVENT_CHARMED:
            case SMART_EVENT_CHARMED_TARGET:
            case SMART_EVENT_CORPSE_REMOVED:
            case SMART_EVENT_AI_INIT:
            case SMART_EVENT_TRANSPORT_ADDPLAYER:
            case SMART_EVENT_TRANSPORT_REMOVE_PLAYER:
            case SMART_EVENT_AGGRO:
            case SMART_EVENT_DEATH:
            case SMART_EVENT_EVADE:
            case SMART_EVENT_REACHED_HOME:
            case SMART_EVENT_RESET:
            case SMART_EVENT_QUEST_ACCEPTED:
            case SMART_EVENT_QUEST_OBJ_COMPLETION:
            case SMART_EVENT_QUEST_COMPLETION:
            case SMART_EVENT_QUEST_REWARDED:
            case SMART_EVENT_QUEST_FAIL:
            case SMART_EVENT_JUST_SUMMONED:
            case SMART_EVENT_WAYPOINT_START:
            case SMART_EVENT_WAYPOINT_REACHED:
            case SMART_EVENT_WAYPOINT_PAUSED:
            case SMART_EVENT_WAYPOINT_RESUMED:
            case SMART_EVENT_WAYPOINT_STOPPED:
            case SMART_EVENT_WAYPOINT_ENDED:
            case SMART_EVENT_GOSSIP_SELECT:
            case SMART_EVENT_GOSSIP_HELLO:
            case SMART_EVENT_JUST_CREATED:
            case SMART_EVENT_FOLLOW_COMPLETED:
            case SMART_EVENT_ON_SPELLCLICK:
            case SMART_EVENT_EVENTOBJECT_ONTRIGGER:
            case SMART_EVENT_ON_TAXIPATHTO:
            case SMART_EVENT_EVENTOBJECT_OFFTRIGGER:
            case SMART_EVENT_ON_APPLY_OR_REMOVE_AURA:
                break;
            default:
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Not handled event_type(%u), Entry %ld SourceType %u Event %u Action %u, skipped.", e.GetEventType(), e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                return false;
        }
    }

    switch (e.GetActionType())
    {
        case SMART_ACTION_SET_FACTION:
            if (e.action.faction.factionID && !sFactionTemplateStore.LookupEntry(e.action.faction.factionID))
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Faction %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.faction.factionID);
                return false;
            }
            break;
        case SMART_ACTION_MORPH_TO_ENTRY_OR_MODEL:
        case SMART_ACTION_MOUNT_TO_ENTRY_OR_MODEL:
            if (e.action.morphOrMount.creature || e.action.morphOrMount.model)
            {
                if (e.action.morphOrMount.creature > 0 && !sObjectMgr->GetCreatureTemplate(e.action.morphOrMount.creature))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Creature entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.morphOrMount.creature);
                    return false;
                }

                if (e.action.morphOrMount.model)
                {
                    if (e.action.morphOrMount.creature)
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u has ModelID set with also set CreatureId, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                        return false;
                    }
                    if (!sCreatureDisplayInfoStore.LookupEntry(e.action.morphOrMount.model))
                    {
                        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Model id %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.morphOrMount.model);
                        return false;
                    }
                }
            }
            break;
        case SMART_ACTION_SOUND:
            if (e.action.sound.range > TEXT_RANGE_WORLD)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses invalid Text Range %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.sound.range);
                return false;
            }
            break;
        case SMART_ACTION_SET_EMOTE_STATE:
        case SMART_ACTION_PLAY_EMOTE:
            if (!IsEmoteValid(e, e.action.emote.emote))
                return false;
            break;
        case SMART_ACTION_PLAY_ANIMKIT:
            if (e.action.animKit.animKit && !IsAnimKitValid(e, e.action.animKit.animKit))
                return false;

            if (e.action.animKit.type > 3)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry " SI64FMTD " SourceType %u Event %u Action %u uses invalid AnimKit type %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.animKit.type);
                return false;
            }
            break;
        case SMART_ACTION_FAIL_QUEST:
        case SMART_ACTION_OFFER_QUEST:
            if (!e.action.quest.quest || !IsQuestValid(e, e.action.quest.quest))
                return false;
            break;
        case SMART_ACTION_CLEAR_QUEST:
            for (auto& questID : e.action.clearQuest.quest)
            {
                if (questID && !IsQuestValid(e, questID))
                    questID = 0;
            }
            break;
        case SMART_ACTION_COMPLETE_QUEST:
            for (auto& questID : e.action.completeQuest.quest)
            {
                if (questID && !IsQuestValid(e, questID))
                    questID = 0;
            }
            break;
        case SMART_ACTION_UNLEARN_SPELL:
            for (auto& entryID : e.action.unlearnSpell.spell)
            {
                if (entryID && !IsSpellValid(e, entryID))
                    entryID = 0;
            }
            break;
        case SMART_ACTION_LEARN_SPELL:
            for (auto& entryID : e.action.learnSpell.spell)
            {
                if (entryID && !IsSpellValid(e, entryID))
                    entryID = 0;
            }
            break;
        case SMART_ACTION_ACTIVATE_TAXI:
            {
                if (!sTaxiPathStore.LookupEntry(e.action.taxi.id))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses invalid Taxi path ID %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.taxi.id);
                    return false;
                }
                break;
            }
        case SMART_ACTION_RANDOM_EMOTE:
            if (e.action.randomEmote.emote1 && !IsEmoteValid(e, e.action.randomEmote.emote1))
                return false;

            if (e.action.randomEmote.emote2 && !IsEmoteValid(e, e.action.randomEmote.emote2))
                return false;

            if (e.action.randomEmote.emote3 && !IsEmoteValid(e, e.action.randomEmote.emote3))
                return false;

            if (e.action.randomEmote.emote4 && !IsEmoteValid(e, e.action.randomEmote.emote4))
                return false;

            if (e.action.randomEmote.emote5 && !IsEmoteValid(e, e.action.randomEmote.emote5))
                return false;

            if (e.action.randomEmote.emote6 && !IsEmoteValid(e, e.action.randomEmote.emote6))
                return false;
            break;
        case SMART_ACTION_ADD_AURA:
        case SMART_ACTION_CAST:
        case SMART_ACTION_INVOKER_CAST:
        case SMART_ACTION_CAST_CUSTOM:
            if (!IsSpellValid(e, e.action.cast.spell))
                return false;
            break;
        case SMART_ACTION_CALL_AREAEXPLOREDOREVENTHAPPENS:
        case SMART_ACTION_CALL_GROUPEVENTHAPPENS:
            if (Quest const* qid = sQuestDataStore->GetQuestTemplate(e.action.quest.quest))
            {
                if (!qid->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u SpecialFlags for Quest entry %u does not include FLAGS_EXPLORATION_OR_EVENT(2), skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.quest.quest);
                    return false;
                }
            }
            else
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Quest entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.quest.quest);
                return false;
            }
            break;
        case SMART_ACTION_SET_EVENT_PHASE:
            if (e.action.setEventPhase.phase >= SMART_EVENT_PHASE_MAX)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u attempts to set phase %u. Phase mask cannot be used past phase %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.setEventPhase.phase, SMART_EVENT_PHASE_MAX-1);
                return false;
            }
            break;
        case SMART_ACTION_INC_EVENT_PHASE:
        {
            if (!e.action.incEventPhase.inc && !e.action.incEventPhase.dec)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u is incrementing phase by 0, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                return false;
            }
            if (e.action.incEventPhase.inc > SMART_EVENT_PHASE_MAX || e.action.incEventPhase.dec > SMART_EVENT_PHASE_MAX)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u attempts to increment phase by too large value, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                return false;
            }
        }
            break;
        case SMART_ACTION_REMOVEAURASFROMSPELL:
            if (e.action.removeAura.spell != 0 && !IsSpellValid(e, e.action.removeAura.spell))
                return false;
            break;
        case SMART_ACTION_RANDOM_PHASE:
            {
                if (e.action.randomPhase.phase1 >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhase.phase2 >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhase.phase3 >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhase.phase4 >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhase.phase5 >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhase.phase6 >= SMART_EVENT_PHASE_MAX)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u attempts to set invalid phase, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                    return false;
                }
            }
            break;
        case SMART_ACTION_RANDOM_PHASE_RANGE:       //PhaseMin, PhaseMax
            {
                if (e.action.randomPhaseRange.phaseMin >= SMART_EVENT_PHASE_MAX ||
                    e.action.randomPhaseRange.phaseMax >= SMART_EVENT_PHASE_MAX)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u attempts to set invalid phase, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
                    return false;
                }
                if (!IsMinMaxValid(e, e.action.randomPhaseRange.phaseMin, e.action.randomPhaseRange.phaseMax))
                    return false;
                break;
            }
        case SMART_ACTION_SUMMON_CREATURE:
            if (!IsCreatureValid(e, e.action.summonCreature.creature))
                return false;
            if (e.action.summonCreature.type < TEMPSUMMON_TIMED_OR_DEAD_DESPAWN || e.action.summonCreature.type > TEMPSUMMON_MANUAL_DESPAWN)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses incorrect TempSummonType %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.summonCreature.type);
                return false;
            }
            break;
        case SMART_ACTION_SUMMON_CREATURE_IN_PERS_VISIBILITY:
            if (!IsCreatureValid(e, e.action.sumCreaturePV.creature))
                return false;
            break;
        case SMART_ACTION_CALL_KILLEDMONSTER:
            if (!IsCreatureValid(e, e.action.killedMonster.creature))
                return false;
            break;
        case SMART_ACTION_UPDATE_TEMPLATE:
            if (e.action.updateTemplate.creature && !IsCreatureValid(e, e.action.updateTemplate.creature))
                return false;
            break;
        case SMART_ACTION_SET_SHEATH:
            if (e.action.setSheath.sheath && e.action.setSheath.sheath >= MAX_SHEATH_STATE)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses incorrect Sheath state %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.setSheath.sheath);
                return false;
            }
            break;
        case SMART_ACTION_SET_REACT_STATE:
            {
                if (e.action.react.state > REACT_AGGRESSIVE)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Creature %ld Event %u Action %u uses invalid React State %u, skipped.", e.entryOrGuid, e.event_id, e.GetActionType(), e.action.react.state);
                    return false;
                }
                break;
            }
        case SMART_ACTION_SUMMON_GO:
            if (!IsGameObjectValid(e, e.action.summonGO.entry))
                return false;
            break;
        case SMART_ACTION_ADD_ITEM:
        case SMART_ACTION_REMOVE_ITEM:
            if (!IsItemValid(e, e.action.item.entry))
                return false;

            if (!NotNULL(e, e.action.item.count))
                return false;
            break;
        case SMART_ACTION_TELEPORT:
            if (!sMapStore.LookupEntry(e.action.teleport.mapID))
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Map entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.teleport.mapID);
                return false;
            }
            break;
        case SMART_ACTION_INSTALL_AI_TEMPLATE:
            if (e.action.installTtemplate.id >= SMARTAI_TEMPLATE_END)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Creature %ld Event %u Action %u uses non-existent AI template id %u, skipped.", e.entryOrGuid, e.event_id, e.GetActionType(), e.action.installTtemplate.id);
                return false;
            }
            break;
        case SMART_ACTION_WP_STOP:
            if (e.action.wpStop.quest && !IsQuestValid(e, e.action.wpStop.quest))
                return false;
            break;
        case SMART_ACTION_WP_START:
            {
                if (!sSmartWaypointMgr->GetPath(e.action.wpStart.pathID))
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Creature %ld Event %u Action %u uses non-existent WaypointPath id %u, skipped.", e.entryOrGuid, e.event_id, e.GetActionType(), e.action.wpStart.pathID);
                    return false;
                }
                if (e.action.wpStart.quest && !IsQuestValid(e, e.action.wpStart.quest))
                    return false;
                if (e.action.wpStart.reactState > REACT_AGGRESSIVE)
                {
                    TC_LOG_ERROR("sql.sql", "SmartAIMgr: Creature %ld Event %u Action %u uses invalid React State %u, skipped.", e.entryOrGuid, e.event_id, e.GetActionType(), e.action.wpStart.reactState);
                    return false;
                }
                break;
            }
        case SMART_ACTION_CREATE_TIMED_EVENT:
        {
            if (!IsMinMaxValid(e, e.action.timeEvent.min, e.action.timeEvent.max))
                return false;

            if (!IsMinMaxValid(e, e.action.timeEvent.repeatMin, e.action.timeEvent.repeatMax))
                return false;
            break;
        }
        case SMART_ACTION_SET_POWER:
        case SMART_ACTION_ADD_POWER:
        case SMART_ACTION_REMOVE_POWER:
        {
            if (e.action.power.powerType >= MAX_POWERS)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry " SI64FMTD " SourceType %u Event %u Action %u uses non-existent Power %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), e.action.power.powerType);
                return false;
            }
            break;
        }

        case SMART_ACTION_SET_SPEED:
        {
            if (e.action.setSpeed.type >= MAX_MOVE_TYPE)
            {
                TC_LOG_ERROR("sql.sql", "SmartAIMgr: SMART_ACTION_SET_SPEED have wrong speed type %u for creature " SI64FMTD ", skipped", e.action.setSpeed.type, e.entryOrGuid);
                return false;
            }

            break;
        }
        case SMART_ACTION_FOLLOW:
        case SMART_ACTION_SET_ORIENTATION:
        case SMART_ACTION_STORE_TARGET_LIST:
        case SMART_ACTION_EVADE:
        case SMART_ACTION_FLEE_FOR_ASSIST:
        case SMART_ACTION_DIE:
        case SMART_ACTION_SET_IN_COMBAT_WITH_ZONE:
        case SMART_ACTION_SET_ACTIVE:
        case SMART_ACTION_STORE_VARIABLE_DECIMAL:
        case SMART_ACTION_WP_RESUME:
        case SMART_ACTION_KILL_UNIT:
        case SMART_ACTION_SET_INVINCIBILITY_HP_LEVEL:
        case SMART_ACTION_RESET_GOBJECT:
        case SMART_ACTION_ATTACK_START:
        case SMART_ACTION_THREAT_ALL_PCT:
        case SMART_ACTION_THREAT_SINGLE_PCT:
        case SMART_ACTION_SET_INST_DATA:
        case SMART_ACTION_SET_INST_DATA64:
        case SMART_ACTION_AUTO_ATTACK:
        case SMART_ACTION_ALLOW_COMBAT_MOVEMENT:
        case SMART_ACTION_CALL_FOR_HELP:
        case SMART_ACTION_SET_DATA:
        case SMART_ACTION_MOVE_FORWARD:
        case SMART_ACTION_SET_VISIBILITY:
        case SMART_ACTION_WP_PAUSE:
        case SMART_ACTION_SET_FLY:
        case SMART_ACTION_SET_RUN:
        case SMART_ACTION_SET_SWIM:
        case SMART_ACTION_FORCE_DESPAWN:
        case SMART_ACTION_SET_INGAME_PHASE_MASK:
        case SMART_ACTION_SET_UNIT_FLAG:
        case SMART_ACTION_REMOVE_UNIT_FLAG:
        case SMART_ACTION_PLAYMOVIE:
        case SMART_ACTION_MOVE_TO_POS:
        case SMART_ACTION_RESPAWN_TARGET:
        case SMART_ACTION_CLOSE_GOSSIP:
        case SMART_ACTION_EQUIP:
        case SMART_ACTION_TRIGGER_TIMED_EVENT:
        case SMART_ACTION_REMOVE_TIMED_EVENT:
        case SMART_ACTION_OVERRIDE_SCRIPT_BASE_OBJECT:
        case SMART_ACTION_RESET_SCRIPT_BASE_OBJECT:
        case SMART_ACTION_ACTIVATE_GOBJECT:
        case SMART_ACTION_CALL_SCRIPT_RESET:
        case SMART_ACTION_NONE:
        case SMART_ACTION_CALL_TIMED_ACTIONLIST:
        case SMART_ACTION_SET_NPC_FLAG:
        case SMART_ACTION_ADD_NPC_FLAG:
        case SMART_ACTION_REMOVE_NPC_FLAG:
        case SMART_ACTION_TALK:
        case SMART_ACTION_SIMPLE_TALK:
        case SMART_ACTION_CROSS_CAST:
        case SMART_ACTION_CALL_RANDOM_TIMED_ACTIONLIST:
        case SMART_ACTION_CALL_RANDOM_RANGE_TIMED_ACTIONLIST:
        case SMART_ACTION_RANDOM_MOVE:
        case SMART_ACTION_SET_UNIT_FIELD_BYTES_1:
        case SMART_ACTION_REMOVE_UNIT_FIELD_BYTES_1:
        case SMART_ACTION_INTERRUPT_SPELL:
        case SMART_ACTION_SEND_GO_CUSTOM_ANIM:
        case SMART_ACTION_SET_DYNAMIC_FLAG:
        case SMART_ACTION_ADD_DYNAMIC_FLAG:
        case SMART_ACTION_REMOVE_DYNAMIC_FLAG:
        case SMART_ACTION_JUMP_TO_POS:
        case SMART_ACTION_SEND_GOSSIP_MENU:
        case SMART_ACTION_GO_SET_LOOT_STATE:
        case SMART_ACTION_SEND_TARGET_TO_TARGET:
        case SMART_ACTION_SET_HOME_POS:
        case SMART_ACTION_SET_HEALTH_REGEN:
        case SMART_ACTION_SET_ROOT:
        case SMART_ACTION_BOSS_EVADE:
        case SMART_ACTION_BOSS_ANOUNCE:
        case SMART_ACTION_MOVE_Z:
        case SMART_ACTION_SET_KD:
        case SMART_ACTION_PLAY_SPELL_VISUAL_KIT:
        case SMART_ACTION_SET_GO_FLAG:
        case SMART_ACTION_ADD_GO_FLAG:
        case SMART_ACTION_REMOVE_GO_FLAG:
        case SMART_ACTION_SUMMON_CREATURE_GROUP:
        case SMART_ACTION_RISE_UP:
        case SMART_ACTION_SET_SCENATIO_ID:
        case SMART_ACTION_UPDATE_ACHIEVEMENT_CRITERIA:
        case SMART_ACTION_SUMMON_CONVERSATION:
        case SMART_ACTION_SUMMON_ADD_PLR_PERSONNAL_VISIBILE:
        case SMART_ACTION_SUMMON_ARIATRIGGER:
        case SMART_ACTION_DISABLE_EVADE:
        case SMART_ACTION_SET_CAN_FLY:
        case SMART_ACTION_JOIN_LFG:
        case SMART_ACTION_SUMMON_SCENE:
        case SMART_ACTION_MOD_CURRENCY:
        case SMART_ACTION_CIRCLE_PATH:
        case SMART_ACTION_SET_OVERRIDE_ZONE_LIGHT:
        case SMART_ACTION_IGNORE_PATHFINDING:
        case SMART_ACTION_START_TIMED_ACHIEVEMENT:
        case SMART_ACTION_SEND_GO_VISUAL_ID:
        case SMART_ACTION_SET_HEALTH_IN_PERCENT:
            break;
        default:
            TC_LOG_ERROR("sql.sql", "SmartAIMgr: Not handled action_type(%u), event_type(%u), Entry %ld SourceType %u Event %u, skipped.", e.GetActionType(), e.GetEventType(), e.entryOrGuid, e.GetScriptType(), e.event_id);
            return false;
    }

    return true;
}

bool SmartAIMgr::NotNULL(SmartScriptHolder const& e, uint32 data)
{
    if (!data)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u Parameter can not be NULL, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType());
        return false;
    }
    return true;
}

bool SmartAIMgr::IsCreatureValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sObjectMgr->GetCreatureTemplate(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Creature entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsQuestValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sQuestDataStore->GetQuestTemplate(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Quest entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsGameObjectValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sObjectMgr->GetGameObjectTemplate(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent GameObject entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsSpellValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sSpellMgr->GetSpellInfo(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Spell entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        //WorldDatabase.PExecute("DELETE FROM `smart_scripts` WHERE `entryorguid` = %u", e.entryOrGuid);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsItemValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sObjectMgr->GetItemTemplate(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Item entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsTextEmoteValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sEmotesTextStore.LookupEntry(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Text Emote entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsEmoteValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sEmotesStore.LookupEntry(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent Emote entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsAreaTriggerValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sAreaTriggerStore.LookupEntry(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %ld SourceType %u Event %u Action %u uses non-existent AreaTrigger entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

bool SmartAIMgr::IsAnimKitValid(SmartScriptHolder const& e, uint32 entry)
{
    if (!sAnimKitStore.LookupEntry(entry))
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry " SI64FMTD " SourceType %u Event %u Action %u uses non-existent AnimKit entry %u, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
        return false;
    }
    return true;
}

/*bool SmartAIMgr::IsTextValid(SmartScriptHolder const& e, uint32 id)  // unused
{
    bool error = false;
    uint32 entry = 0;
    if (e.entryOrGuid >= 0)
        entry = uint32(e.entryOrGuid);
    else {
        entry = uint32(abs(e.entryOrGuid));
        CreatureData const* data = sObjectMgr->GetCreatureData(entry);
        if (!data)
        {
            TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %d SourceType %u Event %u Action %u using non-existent Creature guid %d, skipped.", e.entryOrGuid, e.GetScriptType(), e.event_id, e.GetActionType(), entry);
            return false;
        }
        else
            entry = data->id;
    }
    if (!entry || !sCreatureTextMgr->TextExist(entry, uint8(id)))
        error = true;
    if (error)
    {
        TC_LOG_ERROR("sql.sql", "SmartAIMgr: Entry %d SourceType %u Event %u Action %u using non-existent Text id %d, skipped.", e.entryOrGuid, e.GetScriptType(), e.source_type, e.GetActionType(), id);
        return false;
    }
    return true;
}*/

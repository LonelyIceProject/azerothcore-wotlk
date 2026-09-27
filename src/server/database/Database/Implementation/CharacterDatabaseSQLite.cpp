/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CharacterDatabase.h"
#include "DatabaseBackend.h"

void CharacterDatabaseConnection::DoPrepareStatementOverrides()
{
    OverrideStatement(DatabaseBackend::SQLite, CHAR_DEL_ITEM_BOP_TRADE,
        "DELETE FROM item_soulbound_trade_data WHERE rowid = (SELECT rowid FROM item_soulbound_trade_data WHERE itemGuid = ? LIMIT 1)");
    OverrideStatement(DatabaseBackend::SQLite, CHAR_DEL_ITEMCONTAINER_SINGLE_ITEM,
        "DELETE FROM item_loot_storage WHERE rowid = (SELECT rowid FROM item_loot_storage WHERE containerGUID = ? AND itemid = ? AND count = ? AND item_index = ? LIMIT 1)");
    OverrideStatement(DatabaseBackend::SQLite, CHAR_DEL_RECOVERY_ITEM,
        "DELETE FROM recovery_item WHERE rowid = (SELECT rowid FROM recovery_item WHERE Guid = ? AND ItemEntry = ? AND Count = ? ORDER BY Id DESC LIMIT 1)");
    OverrideStatement(DatabaseBackend::SQLite, CHAR_UPD_QUEST_TRACK_GM_COMPLETE,
        "UPDATE quest_tracker SET completed_by_gm = 1 WHERE rowid = (SELECT rowid FROM quest_tracker WHERE id = ? AND character_guid = ? ORDER BY quest_accept_time DESC LIMIT 1)");
    OverrideStatement(DatabaseBackend::SQLite, CHAR_UPD_QUEST_TRACK_COMPLETE_TIME,
        "UPDATE quest_tracker SET quest_complete_time = NOW() WHERE rowid = (SELECT rowid FROM quest_tracker WHERE id = ? AND character_guid = ? ORDER BY quest_accept_time DESC LIMIT 1)");
    OverrideStatement(DatabaseBackend::SQLite, CHAR_UPD_QUEST_TRACK_ABANDON_TIME,
        "UPDATE quest_tracker SET quest_abandon_time = NOW() WHERE rowid = (SELECT rowid FROM quest_tracker WHERE id = ? AND character_guid = ? ORDER BY quest_accept_time DESC LIMIT 1)");
}

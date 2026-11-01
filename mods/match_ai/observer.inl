namespace
{
    constexpr uint32_t kObserverBattleState = 22;
    constexpr size_t kObserverAppState = 0x4A0;
    constexpr size_t kObserverUnitManagerPlayerCount = 0x8C;
    constexpr size_t kObserverUnitAiEnabled = 0x326C;
    constexpr size_t kObserverUnitTargetObject = 0x3130;
    constexpr size_t kObserverObjectHp = 0x38;
    constexpr size_t kObserverOldStringSize = 0x10;
    constexpr size_t kObserverOldStringCapacity = 0x14;
    constexpr size_t kObserverOldStringInlineCapacity = 7;

    bool CopyOldWString(
        const uint8_t* value,
        wchar_t* output,
        size_t outputCount,
        uint16_t* copiedLength)
    {
        if (!value || !output || outputCount == 0 || !copiedLength)
            return false;
        const size_t length = *reinterpret_cast<const uint32_t*>(value + kObserverOldStringSize);
        const size_t capacity = *reinterpret_cast<const uint32_t*>(value + kObserverOldStringCapacity);
        if (length > capacity || length >= outputCount || capacity > 0x10000)
            return false;
        const wchar_t* text = capacity <= kObserverOldStringInlineCapacity
                                  ? reinterpret_cast<const wchar_t*>(value)
                                  : *reinterpret_cast<const wchar_t* const*>(value);
        if (!text)
            return false;
        for (size_t i = 0; i < length; ++i)
            output[i] = text[i];
        output[length] = L'\0';
        *copiedLength = static_cast<uint16_t>(length);
        return true;
    }

    bool CopyClientSnapshot(SdgoPvpAiClientSnapshotV1* output)
    {
        if (!output || output->size < sizeof(SdgoPvpAiClientSnapshotV1) ||
            output->version != SDGO_PVP_AI_CLIENT_OBSERVER_VERSION)
            return false;

        SdgoPvpAiClientSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
        snapshot.received_battle_start_count =
            static_cast<uint32_t>(InterlockedCompareExchange(&g_receivedBattleStartCount, 0, 0));
        snapshot.received_battle_start_mode =
            static_cast<uint16_t>(InterlockedCompareExchange(&g_receivedBattleStartMode, 0, 0));
        snapshot.lua_error_count =
            static_cast<uint32_t>(InterlockedCompareExchange(&g_luaErrors, 0, 0));
        snapshot.lua_error_observer_available =
            InterlockedCompareExchange(&g_luaErrorObserverAvailable, 0, 0) != 0 ? 1 : 0;
        bool coherentCreateReply = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const LONG before = InterlockedCompareExchange(&g_createReplyVersion, 0, 0);
            if (before & 1) continue;
            snapshot.create_reply_sequence = static_cast<uint32_t>(InterlockedCompareExchange(&g_createReplySequence, 0, 0));
            snapshot.create_reply_room_id = static_cast<uint32_t>(InterlockedCompareExchange(&g_createReplyRoom, 0, 0));
            const uint32_t details = static_cast<uint32_t>(InterlockedCompareExchange(&g_createReplyDetails, 0, 0));
            snapshot.create_reply_opcode = static_cast<uint16_t>(details);
            snapshot.create_reply_error = static_cast<uint16_t>(details >> 16);
            const LONG after = InterlockedCompareExchange(&g_createReplyVersion, 0, 0);
            if (before == after && !(after & 1)) { coherentCreateReply = true; break; }
        }
        if (!coherentCreateReply) return false;

        for (const StartDeck& deck : g_startDecks) {
            if (!deck.pilotId || snapshot.received_ai_deck_count >= 12)
                continue;
            SdgoPvpAiClientStartDeckSnapshotV1& copied =
                snapshot.received_ai_decks[snapshot.received_ai_deck_count++];
            copied.pilot_id = deck.pilotId;
            copied.points = deck.points;
            copied.unit_count = static_cast<uint8_t>(deck.units);
            copied.unit_count_valid =
                deck.units >= 1 && deck.units <= 4 ? 1 : 0;
            for (int unit = 0; unit < 4; ++unit)
                copied.npc_ids[unit] = deck.npc[unit];
        }

        uint8_t* app = g_gameApp ? g_gameApp() : nullptr;
        if (!app) {
            *output = snapshot;
            return false;
        }
        snapshot.native_app_state = *reinterpret_cast<const uint32_t*>(app + kObserverAppState);
        uint8_t* profile = g_profilePointer ? *g_profilePointer : nullptr;
        if (profile)
            snapshot.profile_pilot_id =
                *reinterpret_cast<const uint32_t*>(profile + 0x138);
        if (snapshot.native_app_state != kObserverBattleState) {
            *output = snapshot;
            return true;
        }

        uint8_t* service = *reinterpret_cast<uint8_t**>(app + kAppGameMain);
        uint8_t* mode = service ? *reinterpret_cast<uint8_t**>(service + kServiceMode) : nullptr;
        if (!mode || mode != g_botMode) {
            *output = snapshot;
            return false;
        }
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        void* unitManager = g_game.unitManager ? *g_game.unitManager : nullptr;
        if (!room || !unitManager) {
            *output = snapshot;
            return false;
        }

        snapshot.room_mode = *reinterpret_cast<const uint16_t*>(room + kModeRoomMode);
        if (!CopyOldWString(
                room,
                snapshot.room_title,
                sizeof(snapshot.room_title) / sizeof(snapshot.room_title[0]),
                &snapshot.room_title_length)) {
            snapshot.room_title[0] = L'\0';
            snapshot.room_title_length = 0;
        }
        snapshot.human_participant_count =
            static_cast<uint32_t>(RecordCount(room, kRoomHumansBegin, kRoomHumansEnd));
        snapshot.ai_participant_count =
            static_cast<uint32_t>(RecordCount(room, kRoomAiBegin, kRoomAiEnd));
        const uint8_t* humanRecords =
            *reinterpret_cast<uint8_t* const*>(room + kRoomHumansBegin);
        const int humanCount =
            RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        for (int i = 0; humanRecords && i < humanCount; ++i) {
            const uint8_t* record = humanRecords + i * kRecordSize;
            if (*reinterpret_cast<const uint32_t*>(record + kRecordServerId) ==
                snapshot.profile_pilot_id) {
                snapshot.local_team = record[kRecordTeam];
                break;
            }
        }
        snapshot.unit_manager_player_count =
            *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(unitManager) + kObserverUnitManagerPlayerCount);

        const size_t count = (std::min)(g_botTable.size(), static_cast<size_t>(12));
        snapshot.bot_count = static_cast<uint32_t>(count);
        for (size_t i = 0; i < count; ++i) {
            const Bot& bot = g_botTable[i];
            SdgoPvpAiClientBotSnapshotV1& copied = snapshot.bots[i];
            copied.pilot_id = bot.pilotId;
            copied.object_id = bot.objectId;
            if (!bot.unit)
                continue;

            copied.npc_id = *reinterpret_cast<const uint32_t*>(bot.unit + kUnitNpcDataId);
            copied.target_object_id =
                *reinterpret_cast<const uint32_t*>(bot.unit + kObserverUnitTargetObject);
            copied.ai_enabled = bot.unit[kObserverUnitAiEnabled] != 0 ? 1 : 0;
            copied.local_control = bot.unit[kUnitLocalControl] != 0 ? 1 : 0;
            copied.remote_copy = bot.unit[kUnitRemote] != 0 ? 1 : 0;
            copied.master_npc = bot.unit[kUnitMasterNpc] != 0 ? 1 : 0;
            const uintptr_t nativeDeckBegin =
                reinterpret_cast<uintptr_t>(
                    *reinterpret_cast<uint8_t**>(bot.unit + kUnitDeckBegin));
            const uintptr_t nativeDeckEnd =
                reinterpret_cast<uintptr_t>(
                    *reinterpret_cast<uint8_t**>(bot.unit + kUnitDeckEnd));
            if (nativeDeckBegin && nativeDeckEnd > nativeDeckBegin) {
                const uintptr_t nativeDeckBytes =
                    nativeDeckEnd - nativeDeckBegin;
                if (nativeDeckBytes % kUnitDeckEntry == 0) {
                    copied.raw_deck_count =
                        static_cast<uint32_t>(
                            nativeDeckBytes / kUnitDeckEntry);
                    if (copied.raw_deck_count <= 4) {
                        copied.native_deck_count_valid = 1;
                        copied.deck_count =
                            static_cast<uint8_t>(copied.raw_deck_count);
                        for (uint32_t deckIndex = 0;
                             deckIndex < copied.raw_deck_count;
                             ++deckIndex) {
                            const uint8_t* unitData =
                                *reinterpret_cast<const uint8_t* const*>(
                                    nativeDeckBegin +
                                    deckIndex * kUnitDeckEntry);
                            if (unitData)
                                copied.native_deck_unit_data_ids[deckIndex] =
                                    *reinterpret_cast<const uint32_t*>(unitData);
                        }
                    }
                }
            }
            copied.valid_registered_object =
                bot.objectId && g_findObject(unitManager, bot.objectId) == bot.unit ? 1 : 0;

            // CTrackingCamera::Update (0x00493F30) follows its unit through
            // unit+0x08 and copies the three floats at scene-node+0x88.
            const uint8_t* node =
                *reinterpret_cast<uint8_t* const*>(bot.unit + kUnitNode);
            if (node) {
                const float* position =
                    reinterpret_cast<const float*>(node + kNodePosition);
                const float x = position[0];
                const float y = position[1];
                const float z = position[2];
                if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
                    copied.position_valid = 1;
                    copied.position_x = x;
                    copied.position_y = y;
                    copied.position_z = z;
                }
            }

            uint8_t* slot = g_game.playerSlot(unitManager, bot.slot);
            uint8_t* record =
                slot ? *reinterpret_cast<uint8_t**>(slot + kSlotRecordCopy) : nullptr;
            if (record) {
                copied.team = record[kRecordTeam];
                copied.colour = record[kRecordColour];
            }

            if (copied.target_object_id) {
                const uint8_t* target = static_cast<const uint8_t*>(
                    g_findObject(unitManager, copied.target_object_id));
                if (target)
                    copied.target_hp =
                        *reinterpret_cast<const int32_t*>(target + kObserverObjectHp);
            }
        }

        *output = snapshot;
        return true;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoPvpAiClientGetSnapshotV1(
    SdgoPvpAiClientSnapshotV1* snapshot)
{
    __try {
        return CopyClientSnapshot(snapshot) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoPvpAiClientGetLuaStatusV1(
    SdgoPvpAiClientLuaStatusV1* status)
{
    __try {
        if (!status || status->size < sizeof(*status) ||
            status->version != SDGO_PVP_AI_CLIENT_OBSERVER_VERSION)
            return 0;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            SdgoPvpAiClientLuaStatusV1 snapshot{};
            snapshot.size = sizeof(snapshot);
            snapshot.version = SDGO_PVP_AI_CLIENT_OBSERVER_VERSION;
            const LONG before = InterlockedCompareExchange(&g_receivedBattleStartCount, 0, 0);
            snapshot.battle_start_sequence = static_cast<uint32_t>(before);
            snapshot.error_count = static_cast<uint32_t>(InterlockedCompareExchange(&g_luaErrors, 0, 0));
            snapshot.observer_available = InterlockedCompareExchange(&g_luaErrorObserverAvailable, 0, 0) != 0;
            if (before != InterlockedCompareExchange(&g_receivedBattleStartCount, 0, 0))
                continue;
            *status = snapshot;
            return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

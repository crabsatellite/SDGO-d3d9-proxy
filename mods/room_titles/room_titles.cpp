// Client integration for portable extended room titles. The original room
// packet and native UI remain in charge of all gameplay and password fields.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "client_state.h"
#include "native_transport.h"
#include "native_ui.h"

namespace {
namespace titles = sdgo::room_titles;
namespace metadata = sdgo::room_metadata;
namespace ui = titles::native_ui;
namespace transport = titles::native_transport;
using SubmitFn = void(__thiscall*)(void*);
using EventFn = int(__thiscall*)(void*, void*);
using NativeWideFn = const wchar_t*(__cdecl*)(UINT, const char*);

SubmitFn g_pvpCreate = nullptr;
SubmitFn g_missionCreate = nullptr;
SubmitFn g_missionSettings = nullptr;
EventFn g_titleEvent = nullptr;
NativeWideFn g_nativeWide = nullptr;
void** g_missionRoom = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
titles::ClientState g_state;
uint32_t g_activeConnection = 0;
uint32_t g_activeRoom = 0;
constexpr uint32_t kBattleDomain = 0x80000000u;
using NoticeFn = void (*)(const wchar_t*);
NoticeFn g_notice = nullptr; // test injection only; production uses the native message box

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

struct Draft {
    ui::Surface surface = ui::Surface::PvpCreate;
    uint16_t opcode = 0;
    uint32_t room = 0;
    std::wstring text;
    std::string utf8;
    std::string native_full;
    std::string native_short;
    metadata::Message request;
};
std::unordered_map<uint32_t, Draft> g_drafts;

struct SavedDraft {
    bool valid = false;
    uint32_t connection = 0;
    uint32_t room = 0;
    std::wstring text;
};
std::array<SavedDraft, 4> g_saved;

uint16_t Word(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

uint32_t Dword(const uint8_t* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void Explain(const wchar_t* text)
{
    if (g_notice) {
        g_notice(text);
        return;
    }
    MessageBoxW(GetActiveWindow(), text, L"SDGO room titles", MB_OK | MB_ICONINFORMATION);
}

bool Utf8(const std::wstring& text, std::string* output)
{
    if (text.empty() || text.size() > 256)
        return false;
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > static_cast<int>(metadata::kMaximumTitleBytes))
        return false;
    std::string encoded(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            encoded.data(), bytes, nullptr, nullptr) != bytes ||
        !metadata::ValidateTitle(encoded.data(), encoded.size()).ok())
        return false;
    output->swap(encoded);
    return true;
}

bool Wide(const std::string& text, std::wstring* output)
{
    if (text.empty())
        return false;
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (units <= 0 || units > 256)
        return false;
    std::wstring decoded(static_cast<size_t>(units), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            decoded.data(), units) != units)
        return false;
    output->swap(decoded);
    return true;
}

bool Projection(const std::wstring& text, std::string* full, std::string* shortened, bool* legacyExact)
{
    if (!ui::LegacyTitleBytes(text, full))
        return false;
    *legacyExact = false;
    if (full->size() <= 20 && g_nativeWide) {
        const wchar_t* roundtrip = g_nativeWide(874, full->c_str());
        if (roundtrip) {
            size_t length = 0;
            while (length <= 256 && roundtrip[length])
                ++length;
            *legacyExact = length == text.size() && length <= 256 &&
                           std::equal(text.begin(), text.end(), roundtrip);
        }
    }
    std::wstring prefix;
    std::string previous;
    for (size_t position = 0; position < text.size();) {
        prefix.push_back(text[position++]);
        if (prefix.back() >= 0xD800 && prefix.back() <= 0xDBFF && position < text.size())
            prefix.push_back(text[position++]);
        std::string encoded;
        if (!ui::LegacyTitleBytes(prefix, &encoded))
            return false;
        if (encoded.size() > 20)
            break;
        previous.swap(encoded);
    }
    if (previous.empty())
        previous = "Room";
    shortened->swap(previous);
    return true;
}

uint32_t SafeRoomId(void** slot)
{
    __try {
        return slot && *slot ? *static_cast<const uint32_t*>(*slot) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}


bool Prepare(ui::Surface surface, const std::wstring& text)
{
    Draft draft;
    draft.surface = surface;
    draft.opcode = surface == ui::Surface::MissionSettings ? 0x09ED :
                   surface == ui::Surface::PvpTitleEdit ? 0x089E : 0x0640;
    draft.text = text;
    bool legacyExact = false;
    if (!Utf8(text, &draft.utf8) || !Projection(text, &draft.native_full, &draft.native_short, &legacyExact)) {
        Explain(L"The room title must contain 1 to 128 characters and no control characters. Your text was kept.");
        return false;
    }
    const wchar_t* error = nullptr;
    {
        Lock lock;
        const uint32_t connection = g_activeConnection;
        g_state.AdvanceTime(connection, GetTickCount64());
        const bool creating = draft.opcode == 0x0640;
        draft.room = creating ? 0 : g_activeRoom;
        if (surface == ui::Surface::MissionSettings) {
            const uint32_t nativeId = SafeRoomId(g_missionRoom);
            if (nativeId && !(nativeId & kBattleDomain))
                draft.room = nativeId;
        }
        auto& saved = g_saved[static_cast<size_t>(surface)];
        saved.valid = true;
        saved.connection = connection;
        saved.room = draft.room;
        saved.text = text;
        const auto pending = g_state.Pending(connection);
        if (pending && pending->status == titles::PendingStatus::Awaiting) {
            error = L"The previous room title is still waiting for server confirmation. Please wait before changing it again.";
        } else if (g_state.IsReady(connection) && g_state.IsNegotiating(connection)) {
            error = L"The room list is still synchronizing. Please try the title again after it finishes. Your text was kept.";
        } else if (!g_state.IsReady(connection)) {
            if (legacyExact) {
                g_state.CancelPending(connection);
                g_drafts.erase(connection);
                saved.valid = false;
                return true;
            }
            error = g_state.IsNegotiating(connection) ?
                L"The server is still checking extended room titles. Please wait for the room list, then retry. Your text was kept." :
                L"This server has not confirmed extended room title support. Ask the server owner to check the matching Mods. Your text was kept.";
        } else if (!creating && (!draft.room || !g_state.Query(connection, draft.room))) {
            error = L"The room identity has not been confirmed yet. Refresh or re-enter this room, then try again. Your text was kept.";
        } else {
            const auto previousDraft = g_drafts.find(connection);
            bool sameVersion = !draft.room;
            if (pending && draft.room) {
                const auto current = g_state.Query(connection, draft.room);
                sameVersion = current && current->key.server_epoch == pending->request.server_epoch &&
                    current->key.generation == pending->request.generation &&
                    current->revision == pending->request.revision;
            }
            const bool retry = pending && previousDraft != g_drafts.end() &&
                sameVersion &&
                previousDraft->second.opcode == draft.opcode &&
                pending->request.title == draft.utf8 && pending->request.room_id == draft.room &&
                (pending->error.code == titles::ClientError::Timeout ||
                 pending->error.code == titles::ClientError::TransportError);
            if (retry)
                draft.request = pending->request;
            else if (pending)
                g_state.CancelPending(connection);
            g_drafts[connection] = std::move(draft);
            return true;
        }
    }
    Explain(error);
    return false;
}

void Submit(SubmitFn original, void* service, ui::Surface surface)
{
    try {
        void* edit = ui::FindRoomNameEdit(service, surface);
        std::wstring title;
        if (!edit || !ui::ReadEditText(edit, &title)) {
            Explain(L"This room title control is not compatible with the installed Mod. The title was not submitted.");
            return;
        }
        if (Prepare(surface, title)) {
            original(service);
            // These three verified builders send synchronously (possibly into
            // the native delay queue). No send means native UI validation
            // stopped the action; do not block unrelated old-room gameplay.
            Lock lock;
            const auto draft = g_drafts.find(g_activeConnection);
            if (draft != g_drafts.end() && draft->second.surface == surface && !draft->second.request.request_id)
                g_drafts.erase(draft);
        }
    } catch (const std::bad_alloc&) {
        Explain(L"There is not enough memory to submit the room title. Your text was not truncated.");
    }
}

void __fastcall PvpCreate(void* self, void*) { Submit(g_pvpCreate, self, ui::Surface::PvpCreate); }
void __fastcall MissionCreate(void* self, void*) { Submit(g_missionCreate, self, ui::Surface::MissionCreate); }
void __fastcall MissionSettings(void* self, void*) { Submit(g_missionSettings, self, ui::Surface::MissionSettings); }

int __fastcall TitleEvent(void* self, void*, void* event)
{
    if (!event || Word(static_cast<const uint8_t*>(event) + 4) != 0x0BDB)
        return g_titleEvent(self, event);
    try {
        uint32_t nativeId = 0;
        bool battleDomain = false;
        std::wstring confirmed;
        if (!ui::ReadTitleBarRoom(self, &battleDomain, &nativeId, &confirmed) || nativeId > 400) {
            Explain(L"The room identity is no longer available. Your title was not submitted.");
            return 1;
        }
        bool restoreCache = false;
        {
            Lock lock;
            g_activeRoom = nativeId | (battleDomain ? kBattleDomain : 0);
            const auto pending = g_state.Pending(g_activeConnection);
            restoreCache = pending && pending->status != titles::PendingStatus::Awaiting;
            const auto record = g_state.Query(g_activeConnection, g_activeRoom);
            if (restoreCache && record && !record->title.empty())
                Wide(record->title, &confirmed);
        }
        std::wstring title;
        if (!ui::ReadNativeString(static_cast<const uint8_t*>(event) + 0x2C, &title))
            return 1;
        // The native handler compares its cache before sending. Restore the
        // confirmed value so an identical failed draft can actually be retried.
        if (restoreCache && !ui::SetTitleBarCachedTitle(self, confirmed))
            return 1;
        if (!Prepare(ui::Surface::PvpTitleEdit, title))
            return 1;
        const int result = g_titleEvent(self, event);
        // An unchanged title or native validation can consume the event without
        // sending. Keep the editable text, but do not leave a phantom operation.
        Lock lock;
        const auto draft = g_drafts.find(g_activeConnection);
        if (draft != g_drafts.end() && draft->second.surface == ui::Surface::PvpTitleEdit &&
            !draft->second.request.request_id)
            g_drafts.erase(draft);
        return result;
    } catch (const std::bad_alloc&) {
        return 1;
    }
}

struct TitleField { size_t size; size_t length; size_t text; };
TitleField Field(uint16_t opcode)
{
    switch (opcode) {
    case 0x0640: return {0x53, 0x15, 0x16};
    case 0x09ED: return {0x3E, 0x12, 0x13};
    case 0x050F: return {0x37, 0x0B, 0x0C};
    case 0x089E: return {0x1D, 7, 8};
    default: return {};
    }
}

transport::SendDecision BeforeSend(uint32_t connection, const uint8_t* packet, size_t size, uint32_t,
                                   std::vector<uint8_t>* replacement)
{
    if (size < 2)
        return transport::SendDecision::Pass;
    const uint16_t opcode = Word(packet);
    if (opcode == 0x08A7) {
        bool blocked = false;
        {
            Lock lock;
            g_state.AdvanceTime(connection, GetTickCount64());
            blocked = !g_state.CanStartBattle(connection) || g_drafts.count(connection) != 0;
        }
        if (blocked) {
            Explain(L"The full room title has not been confirmed. Reopen the title settings to retry or resolve the saved draft before starting.");
            return transport::SendDecision::Reject;
        }
        return transport::SendDecision::Pass;
    }
    Lock lock;
    g_state.AdvanceTime(connection, GetTickCount64());
    const auto parsed = metadata::ParseTrailer(packet, size);
    if (parsed.status == metadata::TrailerStatus::Valid)
        return transport::SendDecision::Pass; // a native deferred queue already owns this extension
    const bool bootstrapCreate = opcode == 0x0640 && size == 0x53 &&
        packet[6] == 2 && packet[7] == 10 && // native single-mission automatic create
        !g_state.IsNegotiating(connection) &&
        !g_state.HasPending(connection) && !g_drafts.count(connection);
    if (((opcode == 0x07FB || opcode == 0x0A08) && size == 0x16) || bootstrapCreate) {
        if (g_drafts.count(connection))
            return transport::SendDecision::Pass;
        const auto hello = g_state.BeginHello(connection, GetTickCount64());
        g_activeConnection = connection;
        if (!hello.request_id)
            return transport::SendDecision::Pass;
        return metadata::EncodePacket(packet, size, hello, replacement) ? transport::SendDecision::Replace :
                                                                         transport::SendDecision::Reject;
    }
    const auto found = g_drafts.find(connection);
    if (found == g_drafts.end() || found->second.opcode != opcode)
        return transport::SendDecision::Pass;
    auto& draft = found->second;
    const auto field = Field(opcode);
    if (size != field.size || !field.size || packet[field.length] > 20 ||
        (opcode == 0x089E && packet[6] != ((draft.room & kBattleDomain) ? 1 : 2)))
        return transport::SendDecision::Reject;
    const size_t incoming = packet[field.length];
    const size_t directLength = std::min<size_t>(draft.native_full.size(), 20);
    const size_t byteLength = std::min<size_t>(draft.native_full.size() & 0xFF, 20);
    if ((incoming != directLength && incoming != byteLength) ||
        std::memcmp(packet + field.text, draft.native_full.data(), incoming) != 0)
        return transport::SendDecision::Pass;
    if (!draft.request.request_id) {
        draft.request = g_state.BeginSetTitle(connection, draft.room, draft.utf8, GetTickCount64());
        if (!draft.request.request_id)
            return transport::SendDecision::Reject;
    } else {
        const auto pending = g_state.Pending(connection);
        if (!pending || pending->request.request_id != draft.request.request_id ||
            (pending->status != titles::PendingStatus::Awaiting &&
             !g_state.RetryPending(connection, draft.request.request_id, GetTickCount64())))
            return transport::SendDecision::Reject;
    }
    std::vector<uint8_t> native(packet, packet + size);
    native[field.length] = static_cast<uint8_t>(draft.native_short.size());
    std::memset(native.data() + field.text, 0, 21);
    std::memcpy(native.data() + field.text, draft.native_short.data(), draft.native_short.size());
    return metadata::EncodePacket(native.data(), native.size(), draft.request, replacement) ?
           transport::SendDecision::Replace : transport::SendDecision::Reject;
}

void AfterSend(uint32_t connection, bool queued, const uint8_t* packet, size_t size, int result)
{
    if (queued || result > 0)
        return;
    metadata::TrailerView view;
    metadata::Message message;
    if (!metadata::DecodePacket(packet, size, &view, &message) || message.kind != metadata::MessageKind::SetTitle)
        return;
    bool failed = false;
    {
        Lock lock;
        const auto pending = g_state.Pending(connection);
        if (pending && pending->status == titles::PendingStatus::Awaiting)
            failed = g_state.FailPending(connection, message.request_id, titles::ClientError::TransportError);
    }
    if (failed)
        Explain(L"The room title could not be sent. Your text was kept; reopen the title settings to retry.");
}

size_t ReceiveSize(uint16_t opcode, const uint8_t* packet = nullptr, size_t size = 0)
{
    switch (opcode) {
    case 0x054E: case 0x09F4: case 0x07AC: case 0x08FB:
    case 0x0709: case 0x0916: return 8; // native request failures
    case 0x0824:
        return packet && size >= 0x13 && packet[0x12] <= 10 ?
               0x13 + static_cast<size_t>(packet[0x12]) * 0x123 : 0x13;
    case 0x0617: return 0x5B5;
    case 0x0A33: return 0x21FC;
    case 0x06C1: return 0x21FC;
    case 0x0550: return 0x0B;
    case 0x0680: return 0x3CE1;
    case 0x086F: return 0x3CE1;
    case 0x0902: return 0x3CE2;
    case 0x0809: return 6;
    case 0x0717: return 0x13E;
    case 0x091F: return 0x13E;
    case 0x06BA: return 0x1C;
    case 0x087A: return 6;
    default: return 0;
    }
}

uint16_t ErrorOpcode(uint16_t request)
{
    switch (request) {
    case 0x0640: return 0x054E;
    case 0x050F: return 0x09F4;
    case 0x089E: return 0x07AC;
    case 0x09ED: return 0x08FB;
    default: return 0;
    }
}

void OnReceive(uint32_t connection, const uint8_t* packet, size_t size, const uint8_t* body, size_t bodySize)
{
    if (!packet || size < 2)
        return;
    const uint16_t opcode = Word(packet);
    bool failed = false;
    {
    Lock lock;
    const auto previous = g_state.Pending(connection);
    if (body && bodySize) {
        metadata::Message message;
        metadata::SnapshotBatch batch;
        if (metadata::DecodeMessageBody(body, bodySize, &message))
            g_state.AcceptMessage(connection, message);
        else if (metadata::DecodeSnapshotBatchBody(body, bodySize, &batch))
            g_state.AcceptBatch(connection, batch);
        if (!g_state.HasPending(connection)) {
            const auto draft = g_drafts.find(connection);
            if (draft != g_drafts.end() && draft->second.request.request_id) {
                g_saved[static_cast<size_t>(draft->second.surface)].valid = false;
                g_drafts.erase(draft);
            }
        }
    } else if ((opcode == 0x0617 || opcode == 0x0824 || opcode == 0x0709 || opcode == 0x0916 ||
                ((opcode == 0x0A33 || opcode == 0x054E) && g_state.IsNegotiating(connection))) &&
               !g_state.HasPending(connection) && !g_drafts.count(connection)) {
        // A bare list response proves this path did not negotiate the extension.
        g_state.DropConnection(connection);
    } else if ((opcode == 0x0A33 || opcode == 0x0809 || opcode == 0x06BA) && g_state.HasPending(connection)) {
        const auto pending = g_state.Pending(connection);
        if (pending)
            g_state.FailPending(connection, pending->request.request_id, titles::ClientError::TransportError);
    } else if (size == 8 && Word(packet + 6) != 0 && g_state.HasPending(connection)) {
        const auto draft = g_drafts.find(connection);
        if (draft != g_drafts.end() && draft->second.request.request_id &&
            ErrorOpcode(draft->second.opcode) == opcode)
            g_state.FailPending(connection, draft->second.request.request_id, titles::ClientError::NativeRejected);
    }
    const auto pending = g_state.Pending(connection);
    failed = previous && previous->status == titles::PendingStatus::Awaiting &&
             pending && pending->status == titles::PendingStatus::Failed;
    if (size >= 10 && (opcode == 0x0A33 || opcode == 0x06C1)) {
        g_activeConnection = connection;
        g_activeRoom = Dword(packet + 6);
    } else if (size >= 10 && (opcode == 0x0550 || opcode == 0x0680 || opcode == 0x086F)) {
        g_activeConnection = connection;
        g_activeRoom = Dword(packet + 6) | kBattleDomain;
    } else if (opcode == 0x087A && connection == g_activeConnection) {
        g_activeRoom = 0;
        g_state.CancelPending(connection);
        g_drafts.erase(connection);
    }
    }
    // A notice may run a Windows message loop. Never show it while owning the
    // title state lock: UI callbacks may re-enter to restore the saved draft.
    if (failed)
        Explain(L"The server did not confirm this room title. Your text was kept; reopen the title settings to retry.");
}

void OnInit(ui::Surface surface, void*, void* edit)
{
    if (!edit)
        return;
    std::wstring value;
    {
        Lock lock;
        const auto& saved = g_saved[static_cast<size_t>(surface)];
        const bool editing = surface == ui::Surface::MissionSettings || surface == ui::Surface::PvpTitleEdit;
        const uint32_t room = surface == ui::Surface::MissionSettings ? SafeRoomId(g_missionRoom) :
                              editing ? g_activeRoom : 0;
        if (saved.valid && saved.connection == g_activeConnection && saved.room == room)
            value = saved.text;
        else if (editing) {
            const auto record = g_state.Query(g_activeConnection, room);
            if (record && !record->title.empty())
                Wide(record->title, &value);
        }
    }
    if (!value.empty())
        ui::SetEditText(edit, value);
}

bool LookupTitle(bool battleDomain, uint32_t nativeId, std::wstring* output)
{
    if (!output || !nativeId || (nativeId & kBattleDomain))
        return false;
    Lock lock;
    const auto record = g_state.Query(g_activeConnection, nativeId | (battleDomain ? kBattleDomain : 0));
    return record && !record->title.empty() && Wide(record->title, output);
}

template <typename T> bool Bind(const SdgoApi* api, SdgoMod* mod, T* output, const char* name)
{
    *output = reinterpret_cast<T>(api->Symbol(mod, name));
    return *output != nullptr;
}
} // namespace

SDGO_MOD_ENTRY
{
    if (!ui::Bind(api, mod) || !Bind(api, mod, &g_nativeWide, "room_titles.ui_legacy_to_wide") ||
        !Bind(api, mod, &g_missionRoom, "room_titles.ui_mission_current_room"))
        return 0;
    if (!ui::InstallInitHooks(&OnInit) || !ui::InstallTitleDisplayHooks(&LookupTitle) ||
        !api->HookCode(mod, "room_titles.ui_pvp_create_submit", reinterpret_cast<void*>(&PvpCreate),
                       reinterpret_cast<void**>(&g_pvpCreate)) ||
        !api->HookCode(mod, "room_titles.ui_mission_create_submit", reinterpret_cast<void*>(&MissionCreate),
                       reinterpret_cast<void**>(&g_missionCreate)) ||
        !api->HookCode(mod, "room_titles.ui_mission_settings_submit", reinterpret_cast<void*>(&MissionSettings),
                       reinterpret_cast<void**>(&g_missionSettings)) ||
        !api->HookSlot(mod, "room_titles.ui_pvp_titlebar_event", reinterpret_cast<void*>(&TitleEvent),
                       reinterpret_cast<void**>(&g_titleEvent)))
        return 0;
    const transport::Callbacks callbacks{&BeforeSend, &AfterSend, &ReceiveSize, &OnReceive};
    if (!transport::Install(api, mod, callbacks))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: bounded full room titles over the native game connection");
    return 1;
}

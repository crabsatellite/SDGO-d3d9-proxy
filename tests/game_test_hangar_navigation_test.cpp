// OFFLINE-ONLY ABI, identity, and transition tests for HangarNavigation.
//
// The fixture models the shipped x86 service/control layouts and invokes the
// production adapter against native callback stubs. It never starts GOnline,
// contacts a server, writes a database, registers a real service, or changes
// the real application state.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/hangar_navigation.h"

namespace
{
using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using LoginDispatchDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);

int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x1000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;
uint8_t g_serviceManagerObject[0x48] = {};
void* g_serviceManager = &g_serviceManagerObject;

uint8_t g_downmenu[0x200] = {};
uint8_t g_mainframe[0x200] = {};
uint8_t g_unitManager[0x200] = {};
uint8_t g_wrongService[0x200] = {};
uint8_t g_downmenuRoot[0x180] = {};
uint8_t g_mainframeRoot = 0;
uint8_t g_unitManagerRoot = 0;
uint8_t g_myRoomControl[0x180] = {};
uint8_t g_otherControl[0x180] = {};
uint8_t g_unitRadioControl[0x180] = {};
uint8_t g_gameRoomControl[0x180] = {};
uint8_t g_makeRoomControl[0x180] = {};
uint8_t g_roomList[0x100] = {},g_serviceHead[12] = {},g_serviceNode[12] = {};
void* g_roomListVtable[32] = {};

void* g_downmenuVtable[32] = {};
void* g_mainframeVtable[32] = {};
void* g_unitManagerVtable[32] = {};
void* g_wrongVtable[32] = {};
uint8_t g_operatorMenu[0x100]={},g_inventoryGrid[0x100]={};
uint8_t g_operatorRoot[0x180]={},g_inventoryRoot[0x180]={};
void* g_operatorVtable[32]={},*g_inventoryVtable[32]={};
bool g_gridPresent=false;
unsigned g_operatorCalls=0;
const wchar_t* ReadNativeString(const void* storage);
uint32_t __stdcall FakeOperatorCallback(void* service,uint32_t event,const void* name,void* control) {
    if(service==g_operatorMenu && event==0x101 && control==g_makeRoomControl &&
       wcscmp(ReadNativeString(name),L"UnitBtn")==0)++g_operatorCalls;
    return 0;
}

NetworkSendDetour g_networkSendDetour = nullptr;
LoginDispatchDetour g_loginDispatchDetour = nullptr;

bool g_downmenuVisible = true;
bool g_mainframeVisible = false;
bool g_unitManagerVisible = false;
bool g_sendHangarRequest = true;
bool g_registerUnitManagerImmediately = false;
bool g_mainframeRegistrationRequested = false;
bool g_descriptorValid = true;
bool g_downmenuCallbackValid = true;
bool g_mainframeCallbackValid = true;
bool g_networkPacketValid = true;
int g_networkResult = 1;
unsigned g_downmenuCalls = 0;
unsigned g_mainframeCalls = 0;
unsigned g_nativeStringConstructors = 0;
unsigned g_nativeStringDestructors = 0;
unsigned g_serviceLookups = 0;
unsigned g_controlLookups = 0;
uint32_t g_lastControlKind = UINT32_MAX;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void DestroyNativeString(void* storage)
{
    if (!storage)
        return;
    auto* bytes = static_cast<uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            bytes + 0x14);
    if (capacity >= 8) {
        void* allocation =
            *reinterpret_cast<void**>(bytes);
        if (allocation)
            HeapFree(
                GetProcessHeap(),
                0,
                allocation);
    }
    std::memset(storage, 0, 0x1C);
}

bool WriteNativeString(
    void* storage,
    const wchar_t* value)
{
    if (!storage || !value)
        return false;
    DestroyNativeString(storage);
    auto* bytes = static_cast<uint8_t*>(storage);
    const size_t length = std::wcslen(value);
    if (length < 8) {
        std::memcpy(
            bytes,
            value,
            (length + 1) * sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) * sizeof(wchar_t);
        auto* allocation =
            static_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    byteCount));
        if (!allocation)
            return false;
        std::memcpy(
            allocation,
            value,
            byteCount);
        *reinterpret_cast<wchar_t**>(bytes) =
            allocation;
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) =
            static_cast<uint32_t>(length);
    }
    *reinterpret_cast<uint32_t*>(
        bytes + 0x10) =
        static_cast<uint32_t>(length);
    return true;
}

const wchar_t* ReadNativeString(
    const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            bytes + 0x14);
    return capacity < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : *reinterpret_cast<
              wchar_t* const*>(bytes);
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(
    const void*,
    char* output,
    int)
{
    if (output)
        std::memset(output, '0', 32);
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void* network,
    void*,
    void* stream,
    const void* packet,
    int size,
    char flag)
{
    const auto* bytes =
        static_cast<const uint8_t*>(packet);
    const bool valid =
        network == g_network &&
        stream == &g_stream &&
        flag == 0 &&
        bytes &&
        size == 10 &&
        *reinterpret_cast<const uint16_t*>(
            bytes) == 0x0621 &&
        *reinterpret_cast<const uint32_t*>(
            bytes + 2) == 0 &&
        *reinterpret_cast<const uint32_t*>(
            bytes + 6) == 2;
    if (!valid)
        g_networkPacketValid = false;
    return g_networkResult;
}

int __fastcall FakeLoginDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 0;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    ++g_nativeStringConstructors;
    return WriteNativeString(self, value)
        ? self
        : nullptr;
}

void __fastcall FakeNativeWStringDtor(
    void* self,
    void*)
{
    ++g_nativeStringDestructors;
    DestroyNativeString(self);
}

void* __fastcall FakeNicknameDialogCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* object,
    int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(
    void*,
    void*)
{
}

void __fastcall FakeNicknameDialogConfirm(
    void*,
    void*)
{
}

void* __fastcall FakeServiceFindByName(
    void* self,
    void*,
    const void* descriptor)
{
    ++g_serviceLookups;
    if (self != g_serviceManager ||
        !descriptor) {
        g_descriptorValid = false;
        return nullptr;
    }
    const auto* bytes =
        static_cast<const uint8_t*>(descriptor);
    if (*reinterpret_cast<const uint32_t*>(
            bytes) != 1) {
        g_descriptorValid = false;
        return nullptr;
    }
    const void* nativeName =
        bytes + sizeof(uint32_t);
    const uint32_t size =
        *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(
                nativeName) +
            0x10);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(
                nativeName) +
            0x14);
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name ||
        size != std::wcslen(name) ||
        capacity < size) {
        g_descriptorValid = false;
        return nullptr;
    }
    if (std::wcscmp(name, L"downmenu") == 0)
        return g_downmenuVisible
            ? g_downmenu
            : nullptr;
    if (std::wcscmp(name, L"mainframe") == 0)
        return g_mainframeVisible
            ? g_mainframe
            : nullptr;
    if (std::wcscmp(name, L"unitmgr") == 0)
        return g_unitManagerVisible
            ? g_unitManager
            : nullptr;
    if (wcscmp(name,L"operatormenu")==0) return g_operatorMenu;
    if (wcscmp(name,L"UnitIndexingCubbyhole")==0) return g_gridPresent?g_inventoryGrid:nullptr;
    g_descriptorValid = false;
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t type)
{
    ++g_controlLookups;
    g_lastControlKind = type;
    if (type==4 && root==g_operatorRoot && wcscmp(ReadNativeString(nativeName),L"UnitBtn")==0)return g_makeRoomControl;
    if (type==4 && root==&g_mainframeRoot && wcscmp(ReadNativeString(nativeName),L"MakeRoomBtn")==0)return g_makeRoomControl;
    if (type != 0x0C)
        return nullptr;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (!name)
        return nullptr;
    if (root == &g_downmenuRoot &&
        std::wcscmp(name, L"MyRoomRadio") == 0)
        return g_myRoomControl;
    if (root == &g_downmenuRoot && std::wcscmp(name,L"GameRoomRadio")==0)return g_gameRoomControl;
    if (root == &g_mainframeRoot &&
        std::wcscmp(name, L"UnitRadio") == 0)
        return g_unitRadioControl;
    return nullptr;
}

uint32_t __stdcall FakeDownmenuCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_downmenuCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (service==g_downmenu && event==0x301 && control==g_gameRoomControl && name &&
        std::wcscmp(name,L"GameRoomRadio")==0 && g_sendHangarRequest && g_networkSendDetour) {
        uint8_t packet[10]{};*reinterpret_cast<uint16_t*>(packet)=0x0621;
        *reinterpret_cast<uint32_t*>(packet+6)=2;
        g_networkSendDetour(g_network,nullptr,&g_stream,packet,sizeof(packet),0);
        return 0;
    }
    const bool valid =
        service == g_downmenu &&
        event == 0x301 &&
        control == g_myRoomControl &&
        name &&
        std::wcscmp(
            name,
            L"MyRoomRadio") == 0 &&
        *reinterpret_cast<void**>(
            g_downmenu + 0x44) ==
            *reinterpret_cast<void**>(
                g_myRoomControl + 0x15C);
    if (!valid)
        g_downmenuCallbackValid = false;
    if (valid && g_sendHangarRequest &&
        g_networkSendDetour) {
        uint8_t packet[10] = {};
        *reinterpret_cast<uint16_t*>(packet) =
            0x0621;
        *reinterpret_cast<uint32_t*>(
            packet + 6) = 2;
        g_networkSendDetour(
            g_network,
            nullptr,
            &g_stream,
            packet,
            sizeof(packet),
            0);
    }
    return 0;
}

uint32_t __stdcall FakeMainframeCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_mainframeCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    const bool valid =
        service == g_mainframe &&
        event == 0x301 &&
        control == g_unitRadioControl &&
        name &&
        std::wcscmp(
            name,
            L"UnitRadio") == 0 &&
        *reinterpret_cast<void**>(
            g_mainframe + 0x64) ==
            *reinterpret_cast<void**>(
                g_unitRadioControl + 0x15C);
    if (!valid)
        g_mainframeCallbackValid = false;
    if (valid) {
        g_mainframeRegistrationRequested = true;
        if (g_registerUnitManagerImmediately)
            g_unitManagerVisible = true;
    }
    return 0;
}

uint32_t __stdcall FakeUnitManagerCallback(
    void*,
    uint32_t,
    const void*,
    void*)
{
    return 0;
}

void* LookupSymbol(const char* name)
{
    if(strcmp(name,"flow.operator_menu_vtable")==0)return g_operatorVtable;
    if(strcmp(name,"flow.operator_menu_callback")==0)return reinterpret_cast<void*>(&FakeOperatorCallback);
    if(strcmp(name,"flow.unit_management_vtable")==0)return g_inventoryVtable;
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(
            &FakeGameApp);
    if (std::strcmp(
            name,
            "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginPacketCtor);
    if (std::strcmp(
            name,
            "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(
            &FakePasswordMd5Hex);
    if (std::strcmp(name, "flow.network") == 0)
        return reinterpret_cast<void*>(
            &FakeNetwork);
    if (std::strcmp(
            name,
            "flow.network_send") == 0)
        return reinterpret_cast<void*>(
            &FakeNetworkSend);
    if (std::strcmp(
            name,
            "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginDispatch);
    if (std::strcmp(
            name,
            "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(
            name,
            "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringCtor);
    if (std::strcmp(
            name,
            "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueueUiObject);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogConfirm);
    if (std::strcmp(
            name,
            "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(
            name,
            "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(
            &FakeServiceFindByName);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "flow.hangar_downmenu_vtable") == 0)
        return g_downmenuVtable;
    if (std::strcmp(
            name,
            "flow.hangar_downmenu_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeDownmenuCallback);
    if (std::strcmp(
            name,
            "flow.hangar_mainframe_vtable") == 0)
        return g_mainframeVtable;
    if (std::strcmp(
            name,
            "flow.hangar_mainframe_callback") == 0)
        return reinterpret_cast<void*>(
            &FakeMainframeCallback);
    if (std::strcmp(
            name,
            "flow.battleship_lobby_manager_vtable") ==
        0)
        return g_unitManagerVtable;
    if (std::strcmp(
            name,
            "flow.battleship_lobby_select_business") ==
        0)
        return reinterpret_cast<void*>(
            &FakeUnitManagerCallback);
    return nullptr;
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
{
    return LookupSymbol(name);
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    void* target = LookupSymbol(name);
    if (!target || !detour || !original)
        return 0;
    *original = target;
    if (std::strcmp(
            name,
            "flow.network_send") == 0) {
        g_networkSendDetour =
            reinterpret_cast<
                NetworkSendDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.login_service_dispatch") == 0) {
        g_loginDispatchDetour =
            reinterpret_cast<
                LoginDispatchDetour>(detour);
    }
    return 1;
}

void InitializeVtables()
{
    std::memset(
        g_downmenuVtable,
        0,
        sizeof(g_downmenuVtable));
    std::memset(
        g_mainframeVtable,
        0,
        sizeof(g_mainframeVtable));
    std::memset(
        g_unitManagerVtable,
        0,
        sizeof(g_unitManagerVtable));
    std::memset(
        g_wrongVtable,
        0,
        sizeof(g_wrongVtable));
    g_downmenuVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeDownmenuCallback);
    g_mainframeVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeMainframeCallback);
    g_unitManagerVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeUnitManagerCallback);
}

void ResetFixture(uint32_t appState)
{
    DestroyNativeString(g_gameRoomControl+0x118);DestroyNativeString(g_makeRoomControl+0x118);
    memset(g_serviceManagerObject,0,sizeof(g_serviceManagerObject));
    memset(g_serviceHead,0,sizeof(g_serviceHead));memset(g_serviceNode,0,sizeof(g_serviceNode));memset(g_roomList,0,sizeof(g_roomList));
    *reinterpret_cast<void**>(g_serviceManagerObject+0x44)=g_serviceHead;
    *reinterpret_cast<void**>(g_serviceHead)=g_serviceHead;
    memset(g_gameRoomControl,0,sizeof(g_gameRoomControl));memset(g_makeRoomControl,0,sizeof(g_makeRoomControl));
    DestroyNativeString(
        g_myRoomControl + 0x118);
    DestroyNativeString(
        g_otherControl + 0x118);
    DestroyNativeString(
        g_unitRadioControl + 0x118);
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_downmenu,
        0,
        sizeof(g_downmenu));
    std::memset(
        g_mainframe,
        0,
        sizeof(g_mainframe));
    std::memset(
        g_unitManager,
        0,
        sizeof(g_unitManager));
    std::memset(
        g_wrongService,
        0,
        sizeof(g_wrongService));
    std::memset(
        g_myRoomControl,
        0,
        sizeof(g_myRoomControl));
    std::memset(
        g_otherControl,
        0,
        sizeof(g_otherControl));
    std::memset(
        g_unitRadioControl,
        0,
        sizeof(g_unitRadioControl));

    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = appState;
    *reinterpret_cast<void***>(
        g_downmenu) = g_downmenuVtable;
    *reinterpret_cast<void***>(
        g_mainframe) = g_mainframeVtable;
    *reinterpret_cast<void***>(
        g_unitManager) = g_unitManagerVtable;
    *reinterpret_cast<void***>(
        g_wrongService) = g_wrongVtable;
    *reinterpret_cast<void**>(
        g_downmenu + 0x44) = &g_downmenuRoot;
    *reinterpret_cast<void**>(
        g_downmenu + 0x54) = g_myRoomControl;
    *reinterpret_cast<void**>(g_downmenu+0x48)=g_gameRoomControl;
    *reinterpret_cast<void**>(g_gameRoomControl+0x15C)=&g_downmenuRoot;
    WriteNativeString(g_gameRoomControl+0x118,L"GameRoomRadio");
    g_gameRoomControl[0x162]=g_gameRoomControl[0x167]=1;
    *reinterpret_cast<void**>(
        g_mainframe + 0x64) = &g_mainframeRoot;
    *reinterpret_cast<void**>(
        g_unitManager + 0x64) =
        &g_unitManagerRoot;
    *reinterpret_cast<void**>(
        g_myRoomControl + 0x15C) =
        &g_downmenuRoot;
    *reinterpret_cast<void**>(
        g_unitRadioControl + 0x15C) =
        &g_mainframeRoot;
    WriteNativeString(
        g_myRoomControl + 0x118,
        L"MyRoomRadio");
    WriteNativeString(
        g_otherControl + 0x118,
        L"OtherRadio");
    WriteNativeString(
        g_unitRadioControl + 0x118,
        L"UnitRadio");

    *reinterpret_cast<void**>(
        g_network + 0x14) = &g_stream;
    g_downmenuVisible = true;
    g_mainframeVisible = appState == 5;
    g_unitManagerVisible = false;
    g_sendHangarRequest = true;
    g_registerUnitManagerImmediately = false;
    g_mainframeRegistrationRequested = false;
    g_descriptorValid = true;
    g_downmenuCallbackValid = true;
    g_mainframeCallbackValid = true;
    g_networkPacketValid = true;
    g_networkResult = 1;
    g_downmenuCalls = 0;
    g_mainframeCalls = 0;
    g_nativeStringConstructors = 0;
    g_nativeStringDestructors = 0;
    g_serviceLookups = 0;
    g_controlLookups = 0;
    g_lastControlKind = UINT32_MAX;
}

bool BindNavigation(
    HangarNavigation& navigation,
    SdgoApi& api,
    NativeActions& native)
{
    return navigation.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        &native);
}

void TestBindValidation(
    SdgoApi& api,
    NativeActions& native)
{
    ResetFixture(6);
    void* saved =
        g_downmenuVtable[
            0x38 / sizeof(void*)];
    g_downmenuVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(1);
    HangarNavigation wrongDownmenu;
    Check(
        !BindNavigation(
            wrongDownmenu,
            api,
            native),
        "binding rejects a downmenu vtable whose +0x38 slot is not the callback");
    g_downmenuVtable[
        0x38 / sizeof(void*)] = saved;

    saved =
        g_mainframeVtable[
            0x38 / sizeof(void*)];
    g_mainframeVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(1);
    HangarNavigation wrongMainframe;
    Check(
        !BindNavigation(
            wrongMainframe,
            api,
            native),
        "binding rejects a mainframe vtable whose +0x38 slot is not the callback");
    g_mainframeVtable[
        0x38 / sizeof(void*)] = saved;

    saved =
        g_unitManagerVtable[
            0x38 / sizeof(void*)];
    g_unitManagerVtable[
        0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(1);
    HangarNavigation wrongUnitManager;
    Check(
        !BindNavigation(
            wrongUnitManager,
            api,
            native),
        "binding rejects an actual unitmgr vtable whose +0x38 slot is not SelectBattleShip");
    g_unitManagerVtable[
        0x38 / sizeof(void*)] = saved;
}

void TestFullDelayedFlow(
    SdgoApi& api,
    NativeActions& native)
{
    ResetFixture(6);
    HangarNavigation navigation;
    Check(
        BindNavigation(
            navigation,
            api,
            native),
        "HangarNavigation binds to the verified native symbols");

    uint8_t beforeApp[sizeof(g_app)] = {};
    std::memcpy(
        beforeApp,
        g_app,
        sizeof(g_app));
    Check(
        navigation.EnsureHangar() ==
            HangarNavigation::kOpeningHangar,
        "app 6 invokes the real MyRoomRadio path once");
    Check(
        std::memcmp(
            beforeApp,
            g_app,
            sizeof(g_app)) == 0,
        "the navigation adapter does not write the application object");
    const auto request = navigation.State();
    Check(
        g_downmenuCalls == 1 &&
            g_downmenuCallbackValid &&
            g_networkPacketValid,
        "the exact cached MyRoomRadio control reached its native callback");
    Check(
        request.request_transport_sequence != 0 &&
            request.request_opcode == 0x0621 &&
            request.request_size == 10 &&
            request.request_result != 0,
        "the native transport observer saw the real 10-byte 0621 request");
    Check(
        g_lastControlKind == 0x0C,
        "radio lookup uses native UI kind 0xC");
    Check(
        navigation.EnsureHangar() ==
                HangarNavigation::kWaitingForHangar &&
            g_downmenuCalls == 1,
        "polling app 6 never duplicates the already-sent hangar request");

    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 5;
    g_mainframeVisible = true;
    std::memcpy(
        beforeApp,
        g_app,
        sizeof(g_app));
    Check(
        navigation.EnsureHangar() ==
            HangarNavigation::kOpeningUnitManager,
        "initialized app-5 mainframe invokes UnitRadio when default unitmgr is absent");
    Check(
        std::memcmp(
            beforeApp,
            g_app,
            sizeof(g_app)) == 0,
        "UnitRadio fallback does not write application state");
    Check(
        g_mainframeCalls == 1 &&
            g_mainframeCallbackValid &&
            g_mainframeRegistrationRequested,
        "the genuine UnitRadio control requested original unitmgr construction");
    Check(
        navigation.EnsureHangar() ==
                HangarNavigation::kWaitingForUnitManager &&
            g_mainframeCalls == 1,
        "delayed service registration is polled without a duplicate UnitRadio action");

    g_unitManagerVisible = true;
    *reinterpret_cast<void**>(
        g_unitManager + 0x64) = nullptr;
    Check(
        navigation.EnsureHangar() ==
            HangarNavigation::kWaitingForUnitManager,
        "registered unitmgr remains pending until its native UI root exists");
    *reinterpret_cast<void**>(
        g_unitManager + 0x64) =
        &g_unitManagerRoot;
    Check(
        navigation.EnsureHangar() ==
            HangarNavigation::kReady,
        "success requires app 5 plus registered unitmgr and initialized UI root");
    const auto ready = navigation.State();
    Check(
        ready.mainframe_present &&
            ready.mainframe_root_ready &&
            ready.unit_manager_present &&
            ready.unit_manager_root_ready &&
            ready.unit_radio_invocations == 1,
        "readback exposes the actual mainframe and unitmgr readiness evidence");
    Check(
        g_descriptorValid &&
            g_nativeStringConstructors ==
                g_nativeStringDestructors,
        "every lookup used and destroyed the full type-1 native-wstring descriptor");
}

void TestDefaultAndImmediateConstruction(
    SdgoApi& api,
    NativeActions& native)
{
    ResetFixture(5);
    g_unitManagerVisible = true;
    HangarNavigation alreadyReady;
    Check(
        BindNavigation(
            alreadyReady,
            api,
            native) &&
            alreadyReady.EnsureHangar() ==
                HangarNavigation::kReady &&
            g_mainframeCalls == 0,
        "the client's default-created ready unitmgr is accepted without replaying UnitRadio");

    ResetFixture(5);
    g_registerUnitManagerImmediately = true;
    HangarNavigation immediate;
    Check(
        BindNavigation(
            immediate,
            api,
            native) &&
            immediate.EnsureHangar() ==
                HangarNavigation::kReady &&
            g_mainframeCalls == 1 &&
            g_unitManagerVisible,
        "synchronous original UnitRadio registration can become ready in the same poll");
}

void TestPendingAndIdentityBoundaries(
    SdgoApi& api,
    NativeActions& native)
{
    ResetFixture(4);
    HangarNavigation beforeLobby;
    Check(
        BindNavigation(
            beforeLobby,
            api,
            native) &&
            beforeLobby.EnsureHangar() ==
                HangarNavigation::kWaitingForLobby,
        "states before app 6 wait without touching a control");

    ResetFixture(6);
    g_downmenuVisible = false;
    HangarNavigation noDownmenu;
    Check(
        BindNavigation(
            noDownmenu,
            api,
            native) &&
            noDownmenu.EnsureHangar() ==
                HangarNavigation::kWaitingForLobbyUi,
        "app 6 waits while downmenu is not registered");

    ResetFixture(6);
    *reinterpret_cast<uint32_t*>(
        g_downmenu + 0x60) = 8;
    g_profileStorage[0x62E] = 1;
    HangarNavigation busyLobby;
    Check(
        BindNavigation(
            busyLobby,
            api,
            native) &&
            busyLobby.EnsureHangar() ==
                HangarNavigation::kLobbyBusy &&
            g_downmenuCalls == 0 &&
            busyLobby.State().lobby_busy,
        "the shipped downmenu starter-draw guard is represented as pending without invoking the callback");
    g_profileStorage[0x62E] = 0;

    ResetFixture(5);
    g_mainframeVisible = false;
    HangarNavigation noMainframe;
    Check(
        BindNavigation(
            noMainframe,
            api,
            native) &&
            noMainframe.EnsureHangar() ==
                HangarNavigation::kWaitingForMainframe,
        "app 5 waits while mainframe is not registered");

    ResetFixture(6);
    *reinterpret_cast<void**>(
        g_downmenu + 0x54) = g_otherControl;
    HangarNavigation wrongCached;
    Check(
        BindNavigation(
            wrongCached,
            api,
            native) &&
            wrongCached.EnsureHangar() ==
                HangarNavigation::kDownmenuControlMismatch &&
            g_downmenuCalls == 0,
        "the found MyRoomRadio must equal downmenu's cached +0x54 control");

    ResetFixture(5);
    *reinterpret_cast<void**>(
        g_unitRadioControl + 0x15C) =
        &g_downmenuRoot;
    HangarNavigation wrongParent;
    Check(
        BindNavigation(
            wrongParent,
            api,
            native) &&
            wrongParent.EnsureHangar() ==
                HangarNavigation::kUnitRadioControlMismatch &&
            g_mainframeCalls == 0,
        "UnitRadio must retain mainframe's actual +0x64 UI root as parent");
}

void TestServiceAndTransportFailures(
    SdgoApi& api,
    NativeActions& native)
{
    ResetFixture(6);
    *reinterpret_cast<void***>(
        g_downmenu) = g_wrongVtable;
    HangarNavigation wrongDownmenu;
    Check(
        BindNavigation(
            wrongDownmenu,
            api,
            native) &&
            wrongDownmenu.EnsureHangar() ==
                HangarNavigation::kDownmenuServiceMismatch,
        "a service named downmenu with another vtable fails closed");

    ResetFixture(5);
    *reinterpret_cast<void***>(
        g_mainframe) = g_wrongVtable;
    HangarNavigation wrongMainframe;
    Check(
        BindNavigation(
            wrongMainframe,
            api,
            native) &&
            wrongMainframe.EnsureHangar() ==
                HangarNavigation::kMainframeServiceMismatch,
        "a service named mainframe with another vtable fails closed");

    ResetFixture(5);
    g_unitManagerVisible = true;
    *reinterpret_cast<void***>(
        g_unitManager) = g_wrongVtable;
    HangarNavigation wrongUnitManager;
    Check(
        BindNavigation(
            wrongUnitManager,
            api,
            native) &&
            wrongUnitManager.EnsureHangar() ==
                HangarNavigation::kUnitManagerServiceMismatch,
        "a service named unitmgr with another vtable fails closed");

    ResetFixture(6);
    g_sendHangarRequest = false;
    HangarNavigation noTransport;
    Check(
        BindNavigation(
            noTransport,
            api,
            native) &&
            noTransport.EnsureHangar() ==
                HangarNavigation::kHangarRequestRejected,
        "a callback without a newly observed 0621 transport cannot claim navigation");

    ResetFixture(6);
    g_networkResult = 0;
    HangarNavigation rejectedTransport;
    Check(
        BindNavigation(
            rejectedTransport,
            api,
            native) &&
            rejectedTransport.EnsureHangar() ==
                HangarNavigation::kHangarRequestRejected,
        "a failed original wire send cannot claim navigation");
}

void TestGameRoomRestoration(const SdgoApi& api,NativeActions& native)
{
    ResetFixture(6);HangarNavigation navigation;
    Check(navigation.Bind(&api,reinterpret_cast<SdgoMod*>(1),&native),"game-room restoration binds native callbacks");
    Check(navigation.EnsureGameRoom(g_roomListVtable)==HangarNavigation::kOpeningGameRoom && g_downmenuCalls==1,
        "a missing native room-list consumer enters the actual GameRoomRadio business once");
    Check(navigation.EnsureGameRoom(g_roomListVtable)==HangarNavigation::kWaitingForGameRoom && g_downmenuCalls==1,
        "a transported request alone cannot prove that the native page has been reconstructed");
    *reinterpret_cast<void**>(g_serviceHead)=g_serviceNode;
    *reinterpret_cast<void**>(g_serviceNode)=g_serviceHead;
    *reinterpret_cast<void**>(g_serviceNode+8)=g_roomList;
    *reinterpret_cast<void***>(g_roomList)=g_roomListVtable;
    *reinterpret_cast<void**>(g_roomList+0x64)=&g_mainframeRoot;
    *reinterpret_cast<void**>(g_roomList+0x84)=g_makeRoomControl;
    *reinterpret_cast<void**>(g_makeRoomControl+0x15C)=&g_mainframeRoot;
    WriteNativeString(g_makeRoomControl+0x118,L"MakeRoomBtn");
    Check(navigation.EnsureGameRoom(g_roomListVtable)==HangarNavigation::kWaitingForGameRoom,
        "a registered service with an unavailable MakeRoom button is not ready");
    g_makeRoomControl[0x162]=g_makeRoomControl[0x167]=1;
    Check(navigation.EnsureGameRoom(g_roomListVtable)==HangarNavigation::kReady && g_downmenuCalls==1,
        "only the real registered consumer and visible enabled native control complete the precondition");
    navigation.RequireGameRoomReentry(10);
    Check(navigation.EnsureGameRoom(g_roomListVtable,g_wrongVtable,10)==HangarNavigation::kWaitingForGameRoom,
        "the old native UI initialization generation cannot satisfy a new scene");
    Check(navigation.EnsureGameRoom(g_roomListVtable,g_wrongVtable,11)==HangarNavigation::kReady && g_downmenuCalls==1,
        "a genuine newer native initialization event handles teardown/recreation between two polling frames");
    navigation.RequireGameRoomReentry();
    Check(navigation.EnsureGameRoom(g_roomListVtable,g_wrongVtable)==HangarNavigation::kWaitingForGameRoom && g_downmenuCalls==1,
        "the old still-registered room list cannot satisfy a cross-group scene transition");
    *reinterpret_cast<void**>(g_serviceNode+8)=g_wrongService;
    Check(navigation.EnsureGameRoom(g_roomListVtable,g_wrongVtable)==HangarNavigation::kWaitingForGameRoom && g_downmenuCalls==1,
        "native modal loading blocks new page actions even after old consumer teardown");
    *reinterpret_cast<void**>(g_serviceHead)=g_serviceHead;
    Check(navigation.EnsureGameRoom(g_roomListVtable,g_wrongVtable)==HangarNavigation::kOpeningGameRoom && g_downmenuCalls==2,
        "only observed teardown and finished native loading permit original GameRoomRadio entry");
    ResetFixture(6);HangarNavigation mismatch;
    Check(mismatch.Bind(&api,reinterpret_cast<SdgoMod*>(1),&native),"negative page restoration binds");
    *reinterpret_cast<void**>(g_gameRoomControl+0x15C)=&g_mainframeRoot;
    Check(mismatch.EnsureGameRoom(g_roomListVtable)==HangarNavigation::kDownmenuControlMismatch && g_downmenuCalls==0,
        "a wrong-parent radio cannot invoke the native callback");
}

void TestUnitManagementNavigation(SdgoApi& api,NativeActions& native)
{
    ResetFixture(5);g_unitManagerVisible=true;g_gridPresent=false;g_operatorCalls=0;
    *reinterpret_cast<void***>(g_operatorMenu)=g_operatorVtable;
    *reinterpret_cast<void***>(g_inventoryGrid)=g_inventoryVtable;
    g_operatorVtable[0x38/4]=reinterpret_cast<void*>(&FakeOperatorCallback);
    *reinterpret_cast<void**>(g_operatorMenu+0x80)=g_makeRoomControl;
    *reinterpret_cast<void**>(g_makeRoomControl+0x15C)=g_operatorRoot;
    *reinterpret_cast<void**>(g_inventoryGrid+0x44)=g_inventoryRoot;
    DestroyNativeString(g_makeRoomControl+0x118);WriteNativeString(g_makeRoomControl+0x118,L"UnitBtn");
    g_operatorRoot[0x162]=g_makeRoomControl[0x162]=g_makeRoomControl[0x167]=1;
    HangarNavigation page;Check(BindNavigation(page,api,native),"unit-management native adapter binds");
    Check(page.EnsureUnitManagementPage()==HangarNavigation::kOpeningUnitManagement && g_operatorCalls==1,
        "3D unit viewer is insufficient: invoke the original bottom-menu UnitBtn");
    Check(page.EnsureUnitManagementPage()==HangarNavigation::kWaitingForUnitManagement && g_operatorCalls==1,
        "accepted native callback cannot fabricate page completion or repeat a click");
    g_gridPresent=true;g_inventoryRoot[0x162]=0;
    Check(page.EnsureUnitManagementPage()==HangarNavigation::kWaitingForUnitManagement,"hidden native grid remains pending");
    g_inventoryRoot[0x162]=1;
    Check(page.EnsureUnitManagementPage()==HangarNavigation::kReady,"real visible grid completes native navigation");
    *reinterpret_cast<void***>(g_inventoryGrid)=g_wrongVtable;
    Check(page.EnsureUnitManagementPage()==HangarNavigation::kUnitManagementMismatch,"foreign inventory page cannot satisfy navigation");
    g_gridPresent=false;g_operatorCalls=0;
    *reinterpret_cast<void**>(g_makeRoomControl+0x15C)=&g_mainframeRoot;
    HangarNavigation wrong;Check(BindNavigation(wrong,api,native),"wrong-menu fixture binds");
    Check(wrong.EnsureUnitManagementPage()==HangarNavigation::kOperatorMenuControlMismatch && !g_operatorCalls,
        "wrong-parent menu button never invokes native business");
}

void TestResultNames()
{
    Check(
        std::strcmp(
            HangarNavigation::ResultName(
                HangarNavigation::kReady),
            "ready") == 0 &&
            std::strcmp(
                HangarNavigation::ResultName(
                    HangarNavigation::
                        kUnitManagerServiceMismatch),
                "unit-manager-service-mismatch") ==
                0,
        "result names keep pending and hard-failure states explicit");
}

void TestBottomRadioValidation(SdgoApi& api,NativeActions& native) {
    ResetFixture(6);g_downmenuRoot[0x162]=1;
    HangarNavigation page;Check(BindNavigation(page,api,native),"native bottom-menu adapter binds");
    Check(page.ClickBottomRadio(L"GameRoomRadio",0x48)==HangarNavigation::kReady && g_downmenuCalls==1,
        "general page navigation uses the real radio lookup and original callback");
    g_gameRoomControl[0x167]=0;
    Check(page.ClickBottomRadio(L"GameRoomRadio",0x48)==HangarNavigation::kWaitingForLobbyUi && g_downmenuCalls==1,
        "disabled native radio never dispatches");
    g_gameRoomControl[0x167]=1;
    *reinterpret_cast<void**>(g_gameRoomControl+0x15C)=&g_mainframeRoot;
    Check(page.ClickBottomRadio(L"GameRoomRadio",0x48)==HangarNavigation::kDownmenuControlMismatch && g_downmenuCalls==1,
        "foreign-parent native radio never dispatches");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: HangarNavigation ABI/identity regression; no game, server, or database");

    InitializeVtables();
    ResetFixture(6);

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-hangar";
    config.room_mode = 6;
    config.map_id = 0;

    NativeActions native;
    const bool nativeBound = native.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        config);
    Check(
        nativeBound,
        "NativeActions transport observer bound to offline stubs");
    Check(
        g_networkSendDetour != nullptr &&
            g_loginDispatchDetour != nullptr,
        "offline fixture captured the original transport and login detours");

    if (nativeBound) {
        TestBindValidation(api, native);
        TestFullDelayedFlow(api, native);
        TestDefaultAndImmediateConstruction(
            api,
            native);
        TestPendingAndIdentityBoundaries(
            api,
            native);
        TestServiceAndTransportFailures(
            api,
            native);
        TestResultNames();
        TestGameRoomRestoration(api,native);
        TestUnitManagementNavigation(api,native);
        TestBottomRadioValidation(api,native);
    }

    DestroyNativeString(
        g_myRoomControl + 0x118);
    DestroyNativeString(
        g_otherControl + 0x118);
    DestroyNativeString(
        g_unitRadioControl + 0x118);
    DestroyNativeString(g_gameRoomControl+0x118);DestroyNativeString(g_makeRoomControl+0x118);
    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE HANGAR ABI FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts("OFFLINE HANGAR ABI PASS");
    return 0;
}

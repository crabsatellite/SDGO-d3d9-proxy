#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Drives the shipped VS channel selector or ordinary-lobby join method against
// server-owned channel data. It never constructs a channel row, packet, reply,
// profile, control, or UI event. All mutations are performed by the original
// selector or lobby business methods and reply consumers.
class ChannelActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kAlreadySelected = 2,
        kSelectorOpened = 3,
        kPaging = 4,
        kRequestSent = 5,
        kAccepted = 6,

        kInvalidTarget = 0x100,
        kProfileUnavailable = 0x101,
        kTargetGroupMissing = 0x102,
        kTargetGroupMalformed = 0x103,
        kNoAvailableChannel = 0x104,
        kServiceUnavailable = 0x105,
        kSelectorOpenFailed = 0x106,
        kRowsUnavailable = 0x107,
        kTargetRowMissing = 0x108,
        kNativeRequestPending = 0x109,
        kControlMissing = 0x10A,
        kControlNameInvalid = 0x10B,
        kPageOutOfRange = 0x10C,
        kPageDidNotMove = 0x10D,
        kRequestNotSent = 0x10E,
        kReplyRejected = 0x10F,
        kReplyMismatch = 0x110,
        kReplyNotConsumed = 0x111,
        kNativeFault = 0x112,
        kNativeStateMismatch = 0x113,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t target_group;
        uint32_t selected_group;
        uint32_t target_row;
        uint32_t target_page;
        uint32_t current_page;
        uint32_t max_page;
        uint32_t request_transport_sequence;
        uint32_t reply_sequence;
        uint16_t selected_channel;
        uint16_t target_channel;
        uint16_t reply_opcode;
        uint16_t reply_error;
        uint16_t reply_channel;
        uint8_t request_sent;
        uint8_t reply_consumed;
        uint8_t reserved[2];
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod, NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        profilePointer_ = Symbol<uint8_t**>("flow.profile_pointer");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        vsChannelVtable_ =
            Symbol<void*>("flow.vs_channel_vtable");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>("flow.native_ui_find_control");
        vsChannelClick_ =
            Symbol<VsChannelClickFn>("flow.vs_channel_click_business");
        vsChannelOpen_ =
            Symbol<VsChannelOpenFn>("flow.vs_channel_open_business");
        vsChannelReplyDispatch_ =
            Symbol<ChannelReplyDispatchFn>(
                "flow.vs_channel_reply_dispatch");
        lobbyUiVtable_ =
            Symbol<void*>("flow.lobby_ui_vtable");
        lobbyJoinChannel_ =
            Symbol<LobbyJoinChannelFn>(
                "flow.lobby_join_channel_business");
        lobbyChannelReplyDispatch_ =
            Symbol<ChannelReplyDispatchFn>(
                "flow.lobby_join_channel_reply_dispatch");
        lobbyBattleEntry_=Symbol<LobbyBattleEntryFn>("flow.lobby_battle_channel_entry");

        if (!api_ || !mod_ || !native_ || !profilePointer_ ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !vsChannelVtable_ || !nativeUiFindControl_ ||
            !vsChannelClick_ || !vsChannelOpen_ ||
            !vsChannelReplyDispatch_ || !lobbyUiVtable_ ||
            !lobbyJoinChannel_ || !lobbyChannelReplyDispatch_)
            return false;

        __try {
            void** selectorVtable =
                static_cast<void**>(vsChannelVtable_);
            void** lobbyVtable =
                static_cast<void**>(lobbyUiVtable_);
            if (selectorVtable[kReplyDispatchVtableSlot] !=
                    reinterpret_cast<void*>(vsChannelReplyDispatch_) ||
                selectorVtable[kClickVtableSlot] !=
                    reinterpret_cast<void*>(vsChannelClick_) ||
                selectorVtable[kOpenVtableSlot] !=
                    reinterpret_cast<void*>(vsChannelOpen_) ||
                lobbyVtable[kReplyDispatchVtableSlot] !=
                    reinterpret_cast<void*>(
                        lobbyChannelReplyDispatch_))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (active_ && active_ != this)
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.vs_channel_reply_dispatch",
                reinterpret_cast<void*>(&ObserveVsChannelReply),
                reinterpret_cast<void**>(&vsChannelReplyDispatch_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.lobby_join_channel_reply_dispatch",
                reinterpret_cast<void*>(&ObserveLobbyChannelReply),
                reinterpret_cast<void**>(
                    &lobbyChannelReplyDispatch_))) {
            active_ = nullptr;
            return false;
        }
        if(!api_->HookCode(mod_,"flow.lobby_join_channel_business",reinterpret_cast<void*>(&ObserveNativeLobbyChoice),
            reinterpret_cast<void**>(&lobbyJoinChannel_)))return false;

        bound_ = true;
        Reset();
        return true;
    }

    // Poll this from the normal client update thread. Each call performs at
    // most one native UI action: Open, one page click, or one channel click.
    Result EnsureVsType(uint32_t channelType)
    {
        if (!bound_ || !native_)return Publish(kUnbound,0);
        if (channelType!=1 && channelType!=2)return Publish(kInvalidTarget,0);
        requiredType_=channelType;
        const auto selected=native_->OnboardingState();
        if (selected.native_app_state!=kLobbyAppState)return Publish(kWaitingForLobby,0);
        if(choiceFailed_)return Publish(kNativeStateMismatch,choiceGroup_);
        if (RequestSent())return EnsureVsGroup(static_cast<uint32_t>(InterlockedCompareExchange(&requestTargetGroup_,0,0)));
        if(choiceArmed_)return Publish(kNativeRequestPending,choiceGroup_);
        uint32_t target=0;
        const auto found=FindGroupForType(selected.channel_group,selected.channel_id,&target);
        if (found!=kAccepted)return Publish(found,target);
        return EnsureVsGroup(target);
    }

    Result EnsureVsGroup(uint32_t targetGroup)
    {
        if (!bound_ || !native_)
            return Publish(kUnbound, targetGroup);
        if (targetGroup>=kMaxGroups || (!requiredType_ && targetGroup==0))
            return Publish(kInvalidTarget, targetGroup);

        const NativeActions::OnboardingObservation selected =
            native_->OnboardingState();
        if (selected.native_app_state != kLobbyAppState)
            return Publish(kWaitingForLobby, targetGroup);

        if (RequestSent())
            return PollRequest(targetGroup, selected);

        __try {
            uint8_t* profile = profilePointer_ ? *profilePointer_ : nullptr;
            if (!profile)
                return Publish(kProfileUnavailable, targetGroup);

            GroupView group = {};
            const Result groupResult =
                ReadGroup(profile, targetGroup, &group);
            if (groupResult != kAccepted)
                return Publish(groupResult, targetGroup);

            if (selected.channel_group == targetGroup &&
                selected.channel_id != 0 &&
                GroupContainsChannel(group, selected.channel_id)) {
                targetChannel_ = selected.channel_id;
                return Publish(kAlreadySelected, targetGroup);
            }

            // The loaded lobby's native channel worker supports both groups.
            // The specialized pre-channel selector lists battleship channels.
            if (requiredType_ || targetGroup == kNormalVsGroup)
                return SendThroughLobby(group,targetGroup);

            void* service = ResolveVsChannelService();
            if (!service)
                return SendThroughLobby(group, targetGroup);

            uint8_t* serviceBytes = static_cast<uint8_t*>(service);
            if (serviceBytes[kServiceRequestPending] != 0)
                return Publish(kNativeRequestPending, targetGroup);

            void* root = *reinterpret_cast<void**>(
                serviceBytes + kServiceRoot);
            if (!root) {
                // This is the shipped VS selector Open method from its vtable.
                // It loads the real .uir, rebuilds native rows, and resets its
                // own page through normal client business code.
                vsChannelOpen_(service, 0);
                root = *reinterpret_cast<void**>(
                    serviceBytes + kServiceRoot);
                if (!root)
                    return Publish(kSelectorOpenFailed, targetGroup);
                return Publish(kSelectorOpened, targetGroup);
            }

            Candidate candidate = {};
            const Result candidateResult =
                FindCandidate(group, serviceBytes, &candidate);
            if (candidateResult != kAccepted)
                return Publish(candidateResult, targetGroup);

            targetChannel_ = candidate.channel_id;
            targetRow_ = candidate.row;
            targetPage_ = candidate.row / kRowsPerPage;

            const uint32_t currentPage =
                *reinterpret_cast<const uint32_t*>(
                    serviceBytes + kServicePage);
            const uint32_t maxPage =
                *reinterpret_cast<const uint32_t*>(
                    serviceBytes + kServiceMaxPage);
            currentPage_ = currentPage;
            maxPage_ = maxPage;
            if (targetPage_ > maxPage)
                return Publish(kPageOutOfRange, targetGroup);

            if (currentPage != targetPage_) {
                const wchar_t* name =
                    currentPage < targetPage_ ? L"RightBtn" : L"LeftBtn";
                void* control = FindControl(root, name);
                if (!control)
                    return Publish(kControlMissing, targetGroup);
                if (!ControlHasName(control, name))
                    return Publish(kControlNameInvalid, targetGroup);

                vsChannelClick_(
                    service,
                    kClickMessage,
                    static_cast<uint8_t*>(control) + kControlName,
                    control);
                const uint32_t afterPage =
                    *reinterpret_cast<const uint32_t*>(
                        serviceBytes + kServicePage);
                currentPage_ = afterPage;
                const bool movedTowardTarget =
                    currentPage < targetPage_
                        ? afterPage > currentPage
                        : afterPage < currentPage;
                if (!movedTowardTarget)
                    return Publish(kPageDidNotMove, targetGroup);
                return Publish(kPaging, targetGroup);
            }

            wchar_t buttonName[16] = {};
            const uint32_t slot = candidate.row % kRowsPerPage;
            if (swprintf_s(
                    buttonName,
                    sizeof(buttonName) / sizeof(buttonName[0]),
                    L"ChannelBtn%u",
                    slot) < 0)
                return Publish(kControlNameInvalid, targetGroup);
            void* control = FindControl(root, buttonName);
            if (!control)
                return Publish(kControlMissing, targetGroup);
            if (!ControlHasName(control, buttonName))
                return Publish(kControlNameInvalid, targetGroup);

            const NativeActions::TransportObservation before =
                native_->TransportState();
            const ReplySnapshot replyBefore = ReadReply();
            vsChannelClick_(
                service,
                kClickMessage,
                static_cast<uint8_t*>(control) + kControlName,
                control);
            const NativeActions::TransportObservation after =
                native_->TransportState();
            if (after.send_sequence == before.send_sequence ||
                after.opcode != kJoinChannelRequestOpcode ||
                after.packet_size != kJoinChannelRequestSize ||
                after.result == 0)
                return Publish(kRequestNotSent, targetGroup);

            InterlockedExchange(
                &requestTargetGroup_,
                static_cast<LONG>(targetGroup));
            InterlockedExchange(
                &requestTargetChannel_,
                static_cast<LONG>(candidate.channel_id));
            InterlockedExchange(
                &requestTransportSequence_,
                static_cast<LONG>(after.send_sequence));
            InterlockedExchange(
                &requestReplyBaseline_,
                static_cast<LONG>(replyBefore.sequence));
            InterlockedExchange(&requestSent_, 1);
            return Publish(kRequestSent, targetGroup);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault, targetGroup);
        }
    }

    bool ExcludeSelectedChannel()
    {
        const auto selected=native_ ? native_->OnboardingState():NativeActions::OnboardingObservation{};
        if (!selected.channel_id || excludedCount_>=64)return false;
        if (!IsExcluded(selected.channel_id))excludedChannels_[excludedCount_++]=selected.channel_id;
        InterlockedExchange(&requestSent_,0);
        return true;
    }
    bool TakeSceneReloadRequirement(){const bool value=sceneReloadRequired_;sceneReloadRequired_=false;return value;}

    void Reset()
    {
        InterlockedExchange(&requestSent_, 0);
        InterlockedExchange(&requestTargetGroup_, 0);
        InterlockedExchange(&requestTargetChannel_, 0);
        InterlockedExchange(&requestTransportSequence_, 0);
        InterlockedExchange(&requestReplyBaseline_, 0);
        InterlockedExchange(&lastResult_, kUnbound);
        InterlockedExchange(&lastTargetGroup_, 0);
        targetChannel_ = 0;
        targetRow_ = 0;
        targetPage_ = 0;
        currentPage_ = 0;
        maxPage_ = 0;
        requiredType_ = 0;
        excludedCount_ = 0;
        battleEntryAttempts_=0;
        choiceArmed_=choiceFailed_=sceneReloadRequired_=false;choiceOwner_=nullptr;
    }

    Observation State() const
    {
        Observation observed = {};
        const NativeActions::OnboardingObservation selected =
            native_ ? native_->OnboardingState()
                    : NativeActions::OnboardingObservation{};
        const ReplySnapshot reply = ReadReply();
        observed.result = static_cast<uint32_t>(
            InterlockedCompareExchange(&lastResult_, 0, 0));
        observed.native_app_state = selected.native_app_state;
        observed.target_group = static_cast<uint32_t>(
            InterlockedCompareExchange(&lastTargetGroup_, 0, 0));
        observed.selected_group = selected.channel_group;
        observed.target_row = targetRow_;
        observed.target_page = targetPage_;
        observed.current_page = currentPage_;
        observed.max_page = maxPage_;
        observed.request_transport_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &requestTransportSequence_,
                0,
                0));
        observed.reply_sequence = reply.sequence;
        observed.selected_channel = selected.channel_id;
        observed.target_channel = targetChannel_;
        observed.reply_opcode = reply.opcode;
        observed.reply_error = reply.error;
        observed.reply_channel = reply.channel_id;
        observed.request_sent = RequestSent() ? 1 : 0;
        observed.reply_consumed = reply.consumed;
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForLobby: return "waiting-for-lobby";
        case kAlreadySelected: return "already-selected";
        case kSelectorOpened: return "selector-opened";
        case kPaging: return "paging";
        case kRequestSent: return "request-sent";
        case kAccepted: return "accepted";
        case kInvalidTarget: return "invalid-target";
        case kProfileUnavailable: return "profile-unavailable";
        case kTargetGroupMissing: return "target-group-missing";
        case kTargetGroupMalformed: return "target-group-malformed";
        case kNoAvailableChannel: return "no-available-channel";
        case kServiceUnavailable: return "service-unavailable";
        case kSelectorOpenFailed: return "selector-open-failed";
        case kRowsUnavailable: return "rows-unavailable";
        case kTargetRowMissing: return "target-row-missing";
        case kNativeRequestPending: return "native-request-pending";
        case kControlMissing: return "control-missing";
        case kControlNameInvalid: return "control-name-invalid";
        case kPageOutOfRange: return "page-out-of-range";
        case kPageDidNotMove: return "page-did-not-move";
        case kRequestNotSent: return "request-not-sent";
        case kReplyRejected: return "reply-rejected";
        case kReplyMismatch: return "reply-mismatch";
        case kReplyNotConsumed: return "reply-not-consumed";
        case kNativeFault: return "native-fault";
        case kNativeStateMismatch: return "native-state-mismatch";
        default: return "unknown";
        }
    }

private:
    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr uint32_t kNormalVsGroup = 1;
    static constexpr uint32_t kRowsPerPage = 7;
    static constexpr uint32_t kMaxGroups = 16;
    static constexpr uint32_t kMaxChannelsPerGroup = 64;
    static constexpr uint32_t kMaxRows = 256;
    static constexpr uint32_t kClickMessage = 0x101;
    static constexpr uint16_t kJoinChannelRequestOpcode = 0x04FA;
    static constexpr uint32_t kJoinChannelRequestSize = 0x0E;
    static constexpr uint16_t kJoinChannelReplyOpcode = 0x09DF;

    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kGroupVectorBegin = 0x5B0;
    static constexpr size_t kGroupVectorEnd = 0x5B4;
    static constexpr size_t kGroupStride = 0x764;
    static constexpr size_t kGroupChannelCount = 0x22;
    static constexpr size_t kGroupChannels = 0x24;
    static constexpr size_t kChannelStride = 0x1D;
    static constexpr size_t kChannelId = 0x00;
    // Native channel row: id16, address32, channel-type32, capacity32, ...
    // FUN_006EAA60 reads this field at group+0x2A (channel+0x06).
    static constexpr size_t kChannelType = 0x06;
    static constexpr size_t kChannelCapacity = 0x0A;
    static constexpr size_t kChannelPopulation = 0x0E;
    static constexpr size_t kChannelCategory = 0x16;
    static constexpr uint32_t kVsCategory = 3;

    static constexpr size_t kServiceRoot = 0x60;
    static constexpr size_t kServicePage = 0x1A4;
    static constexpr size_t kServiceMaxPage = 0x1A8;
    static constexpr size_t kServiceRequestPending = 0x1AC;
    static constexpr size_t kServiceRowsBegin = 0x1B0;
    static constexpr size_t kServiceRowsEnd = 0x1B4;
    static constexpr size_t kLobbyRequestState = 0x68;
    static constexpr uint32_t kLobbyJoinPending = 4;
    static constexpr size_t kRowStride = 0x28;
    static constexpr size_t kRowFull = 0x08;
    static constexpr size_t kRowRawChannel = 0x09;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;

    static constexpr size_t kReplyChannel = 0x08;
    static constexpr size_t kReplyError = 0x0C;
    static constexpr size_t kReplyDispatchVtableSlot = 0x28 / sizeof(void*);
    static constexpr size_t kClickVtableSlot = 0x38 / sizeof(void*);
    static constexpr size_t kOpenVtableSlot = 0x3C / sizeof(void*);

    struct GroupView
    {
        const uint8_t* group;
        uint32_t category;
        uint32_t channel_count;
    };

    struct Candidate
    {
        uint16_t channel_id;
        uint32_t row;
    };

    struct ReplySnapshot
    {
        uint32_t sequence;
        uint32_t profile_group;
        uint16_t opcode;
        uint16_t error;
        uint16_t channel_id;
        uint8_t consumed;
        uint8_t reserved;
    };

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using VsChannelClickFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using VsChannelOpenFn = void(__thiscall*)(void*, int);
    using LobbyJoinChannelFn =
        void(__thiscall*)(void*, uint16_t);
    using LobbyBattleEntryFn=void(__thiscall*)(void*);
    using ChannelReplyDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);

    Result Publish(Result result, uint32_t targetGroup) const
    {
        InterlockedExchange(
            &lastTargetGroup_,
            static_cast<LONG>(targetGroup));
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    bool RequestSent() const
    {
        return InterlockedCompareExchange(&requestSent_, 0, 0) != 0;
    }

    Result PollRequest(
        uint32_t targetGroup,
        const NativeActions::OnboardingObservation& selected) const
    {
        const uint32_t requestedGroup = static_cast<uint32_t>(
            InterlockedCompareExchange(&requestTargetGroup_, 0, 0));
        const uint16_t requestedChannel = static_cast<uint16_t>(
            InterlockedCompareExchange(&requestTargetChannel_, 0, 0));
        if (targetGroup != requestedGroup)
            return Publish(kInvalidTarget, targetGroup);

        const ReplySnapshot reply = ReadReply();
        const uint32_t baseline = static_cast<uint32_t>(
            InterlockedCompareExchange(&requestReplyBaseline_, 0, 0));
        if (reply.sequence <= baseline)
            return Publish(kRequestSent, targetGroup);
        if (reply.opcode != kJoinChannelReplyOpcode)
            return Publish(kReplyMismatch, targetGroup);
        if (reply.error != 0)
            return Publish(kReplyRejected, targetGroup);
        if (reply.channel_id != requestedChannel)
            return Publish(kReplyMismatch, targetGroup);
        if (!reply.consumed ||
            reply.profile_group != requestedGroup ||
            selected.channel_group != requestedGroup ||
            selected.channel_id != requestedChannel)
            return Publish(kReplyNotConsumed, targetGroup);
        return Publish(kAccepted, targetGroup);
    }

    Result ReadGroup(
        const uint8_t* profile,
        uint32_t targetGroup,
        GroupView* out) const
    {
        if (!profile || !out)
            return kProfileUnavailable;
        const uint8_t* begin = *reinterpret_cast<uint8_t* const*>(
            profile + kGroupVectorBegin);
        const uint8_t* end = *reinterpret_cast<uint8_t* const*>(
            profile + kGroupVectorEnd);
        if (!begin || !end || end < begin)
            return kTargetGroupMissing;
        const uintptr_t bytes =
            reinterpret_cast<uintptr_t>(end) -
            reinterpret_cast<uintptr_t>(begin);
        if (bytes % kGroupStride != 0)
            return kTargetGroupMalformed;
        const uint32_t groupCount =
            static_cast<uint32_t>(bytes / kGroupStride);
        if (groupCount > kMaxGroups)
            return kTargetGroupMalformed;
        if (targetGroup >= groupCount)
            return kTargetGroupMissing;

        const uint8_t* group = begin + targetGroup * kGroupStride;
        const uint32_t channelCount =
            *reinterpret_cast<const uint16_t*>(
                group + kGroupChannelCount);
        if (channelCount > kMaxChannelsPerGroup)
            return kTargetGroupMalformed;
        out->group = group;
        // A group index is neither the channel's mode nor its channel type.
        out->category = kVsCategory;
        out->channel_count = channelCount;
        return kAccepted;
    }

    bool GroupContainsChannel(
        const GroupView& group,
        uint16_t channelId) const
    {
        for (uint32_t i = 0; i < group.channel_count; ++i) {
            const uint8_t* entry =
                group.group + kGroupChannels + i * kChannelStride;
            if (*reinterpret_cast<const uint16_t*>(
                    entry + kChannelId) == channelId &&
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelCategory) == group.category && !IsExcluded(channelId) &&
                (!requiredType_ || *reinterpret_cast<const uint32_t*>(entry+kChannelType)==requiredType_))
                return true;
        }
        return false;
    }

    Result FindAvailableChannel(
        const GroupView& group,
        uint16_t* out) const
    {
        if (!out)
            return kNoAvailableChannel;
        for (uint32_t i = 0; i < group.channel_count; ++i) {
            const uint8_t* entry =
                group.group + kGroupChannels + i * kChannelStride;
            const uint16_t channelId =
                *reinterpret_cast<const uint16_t*>(
                    entry + kChannelId);
            const uint32_t category =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelCategory);
            const uint32_t capacity =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelCapacity);
            const uint32_t population =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelPopulation);
            if (channelId &&
                !IsExcluded(channelId) &&
                category == group.category &&
                (!requiredType_ || *reinterpret_cast<const uint32_t*>(entry+kChannelType)==requiredType_) &&
                capacity != 0 &&
                population < capacity) {
                *out = channelId;
                return kAccepted;
            }
        }
        return kNoAvailableChannel;
    }

    Result FindGroupForType(uint32_t preferred,uint16_t selected,uint32_t* target) const
    {
        __try {
            const uint8_t* profile=profilePointer_ ? *profilePointer_:nullptr;
            if (!profile)return kProfileUnavailable;
            GroupView group{};
            if (ReadGroup(profile,preferred,&group)==kAccepted && GroupContainsChannel(group,selected)) {
                *target=preferred;return kAccepted;
            }
            for (uint32_t index=0;index<kMaxGroups;++index) {
                const auto result=ReadGroup(profile,index,&group);
                if (result==kTargetGroupMissing)break;
                if (result!=kAccepted)return result;
                uint16_t channel=0;
                if (FindAvailableChannel(group,&channel)==kAccepted){*target=index;return kAccepted;}
            }
            return kNoAvailableChannel;
        }__except(EXCEPTION_EXECUTE_HANDLER){return kNativeFault;}
    }

    bool IsExcluded(uint16_t channel)const
    {
        for (uint32_t i=0;i<excludedCount_;++i)if(excludedChannels_[i]==channel)return true;
        return false;
    }

    Result FindCandidate(
        const GroupView& group,
        const uint8_t* service,
        Candidate* out) const
    {
        if (!service || !out)
            return kRowsUnavailable;
        const uint8_t* rowsBegin =
            *reinterpret_cast<uint8_t* const*>(
                service + kServiceRowsBegin);
        const uint8_t* rowsEnd =
            *reinterpret_cast<uint8_t* const*>(
                service + kServiceRowsEnd);
        if (!rowsBegin || !rowsEnd || rowsEnd < rowsBegin)
            return kRowsUnavailable;
        const uintptr_t rowBytes =
            reinterpret_cast<uintptr_t>(rowsEnd) -
            reinterpret_cast<uintptr_t>(rowsBegin);
        if (rowBytes % kRowStride != 0)
            return kRowsUnavailable;
        const uint32_t rowCount =
            static_cast<uint32_t>(rowBytes / kRowStride);
        if (rowCount == 0 || rowCount > kMaxRows)
            return kRowsUnavailable;

        bool sawVsChannel = false;
        bool sawAvailableChannel = false;
        for (uint32_t i = 0; i < group.channel_count; ++i) {
            const uint8_t* entry =
                group.group + kGroupChannels + i * kChannelStride;
            const uint16_t channelId =
                *reinterpret_cast<const uint16_t*>(entry + kChannelId);
            const uint32_t category =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelCategory);
            if (!channelId || category != group.category)
                continue;
            sawVsChannel = true;

            // The shipped row builder marks a row full when capacity <=
            // population. Select only a strictly non-full server record.
            const uint32_t capacity =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelCapacity);
            const uint32_t population =
                *reinterpret_cast<const uint32_t*>(
                    entry + kChannelPopulation);
            if (capacity == 0 || population >= capacity)
                continue;
            sawAvailableChannel = true;

            for (uint32_t row = 0; row < rowCount; ++row) {
                const uint8_t* nativeRow =
                    rowsBegin + row * kRowStride;
                if (nativeRow[kRowFull] != 0)
                    continue;
                // The native 0x28-byte row embeds the exact 0x1D-byte server
                // channel record at +0x09. Matching all bytes proves this row
                // belongs to group 2 without inventing a group mapping.
                if (memcmp(
                        nativeRow + kRowRawChannel,
                        entry,
                        kChannelStride) != 0)
                    continue;
                out->channel_id = channelId;
                out->row = row;
                return kAccepted;
            }
        }
        if (!sawVsChannel || !sawAvailableChannel)
            return kNoAvailableChannel;
        return kTargetRowMissing;
    }

    Result SendThroughLobby(
        const GroupView& group,
        uint32_t targetGroup)
    {
        void* service = ResolveService(
            L"upmenu",
            lobbyUiVtable_);
        if (!service)
            return Publish(kServiceUnavailable, targetGroup);
        uint8_t* serviceBytes = static_cast<uint8_t*>(service);
        if (*reinterpret_cast<const uint32_t*>(
                serviceBytes + kLobbyRequestState) != 0)
            return Publish(kNativeRequestPending, targetGroup);
        uint16_t channel=0;
        const Result candidate=FindAvailableChannel(group,&channel);
        if(candidate!=kAccepted)return Publish(candidate,targetGroup);
        if (*reinterpret_cast<const uint32_t*>(serviceBytes+0x70)!=1) {
            if (!lobbyBattleEntry_ || battleEntryAttempts_>=4)return Publish(kNativeStateMismatch,targetGroup);
            const uint32_t oldMode=*reinterpret_cast<const uint32_t*>(serviceBytes+0x70);
            choiceArmed_=true;choiceOwner_=service;choiceGroup_=targetGroup;choiceChannel_=channel;
            ++battleEntryAttempts_;
            lobbyBattleEntry_(service);
            if(api_ && api_->Log)api_->Log(mod_,SDGO_LOG_INFO,
                "native VS entry initialized upper-menu mode=%u->%u request_state=%u attempt=%u",
                oldMode,*reinterpret_cast<const uint32_t*>(serviceBytes+0x70),
                *reinterpret_cast<const uint32_t*>(serviceBytes+kLobbyRequestState),battleEntryAttempts_);
            if (*reinterpret_cast<const uint32_t*>(serviceBytes+0x70)!=1)return Publish(kNativeStateMismatch,targetGroup);
            return Publish(kNativeRequestPending,targetGroup);
        }

        targetChannel_ = channel;
        targetRow_ = 0;
        targetPage_ = 0;
        currentPage_ = 0;
        maxPage_ = 0;

        const NativeActions::TransportObservation before =
            native_->TransportState();
        const ReplySnapshot replyBefore = ReadReply();
        sceneReloadRequired_=native_->OnboardingState().channel_group!=targetGroup;
        lobbyJoinChannel_(service, channel);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        const uint32_t requestState =
            *reinterpret_cast<const uint32_t*>(
                serviceBytes + kLobbyRequestState);
        if (after.send_sequence == before.send_sequence ||
            after.opcode != kJoinChannelRequestOpcode ||
            after.packet_size != kJoinChannelRequestSize ||
            after.result == 0)
            return Publish(kRequestNotSent, targetGroup);
        if (requestState != kLobbyJoinPending)
            return Publish(kNativeStateMismatch, targetGroup);

        InterlockedExchange(
            &requestTargetGroup_,
            static_cast<LONG>(targetGroup));
        InterlockedExchange(
            &requestTargetChannel_,
            static_cast<LONG>(channel));
        InterlockedExchange(
            &requestTransportSequence_,
            static_cast<LONG>(after.send_sequence));
        InterlockedExchange(
            &requestReplyBaseline_,
            static_cast<LONG>(replyBefore.sequence));
        InterlockedExchange(&requestSent_, 1);
        return Publish(kRequestSent, targetGroup);
    }

    static void __fastcall ObserveNativeLobbyChoice(void* self,void*,uint16_t automaticChannel)
    {
        auto* actions=active_;
        if(!actions || !actions->lobbyJoinChannel_)return;
        if(!actions->choiceArmed_ || self!=actions->choiceOwner_ ||
            *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self)+0x70)!=1) {
            actions->lobbyJoinChannel_(self,automaticChannel);return;
        }
        // Supply the scenario's choice at the original VS selection callback,
        // after the game's own list/mode initialization. The original worker
        // still constructs/transports the request and consumes the real reply.
        const auto before=actions->native_->TransportState();const auto reply=actions->ReadReply();
        actions->sceneReloadRequired_=actions->native_->OnboardingState().channel_group!=actions->choiceGroup_;
        actions->lobbyJoinChannel_(self,actions->choiceChannel_);
        const auto after=actions->native_->TransportState();actions->choiceArmed_=false;
        if(after.send_sequence==before.send_sequence || after.opcode!=kJoinChannelRequestOpcode ||
            after.packet_size!=kJoinChannelRequestSize || !after.result ||
            *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self)+kLobbyRequestState)!=kLobbyJoinPending) {
            actions->choiceFailed_=true;return;
        }
        actions->targetChannel_=actions->choiceChannel_;
        InterlockedExchange(&actions->requestTargetGroup_,static_cast<LONG>(actions->choiceGroup_));
        InterlockedExchange(&actions->requestTargetChannel_,actions->choiceChannel_);
        InterlockedExchange(&actions->requestTransportSequence_,static_cast<LONG>(after.send_sequence));
        InterlockedExchange(&actions->requestReplyBaseline_,static_cast<LONG>(reply.sequence));
        InterlockedExchange(&actions->requestSent_,1);
        if(actions->api_ && actions->api_->Log)actions->api_->Log(actions->mod_,SDGO_LOG_INFO,
            "native initialized VS choice automatic=%u scenario=%u group=%u original_transport=%u",
            automaticChannel,actions->choiceChannel_,actions->choiceGroup_,after.send_sequence);
    }

    void* ResolveService(
        const wchar_t* name,
        const void* expectedVtable) const
    {
        if (!name || !expectedVtable ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_)
            return nullptr;
        void* manager = *serviceManagedPointer_;
        if (!manager)
            return nullptr;

        alignas(4) uint8_t descriptor[kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            nativeWStringCtor_(
                descriptor + sizeof(uint32_t),
                name);
            constructed = true;
            service = serviceFindByName_(manager, descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor + sizeof(uint32_t));
        }
        if (!service ||
            *reinterpret_cast<void**>(service) != expectedVtable)
            return nullptr;
        return service;
    }

    void* ResolveVsChannelService() const
    {
        return ResolveService(
            L"VSChannelSelect",
            vsChannelVtable_);
    }

    void* FindControl(void* root, const wchar_t* name) const
    {
        if (!root || !name || !nativeUiFindControl_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_)
            return nullptr;
        alignas(4) uint8_t nativeName[kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            nativeWStringCtor_(nativeName, name);
            constructed = true;
            control = nativeUiFindControl_(
                root,
                nativeName,
                4);
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeName);
        }
        return control;
    }

    bool ControlHasName(
        const void* control,
        const wchar_t* expected) const
    {
        if (!control || !expected)
            return false;
        const uint8_t* nativeName =
            static_cast<const uint8_t*>(control) + kControlName;
        const uint32_t size =
            *reinterpret_cast<const uint32_t*>(
                nativeName + kNativeStringSize);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                nativeName + kNativeStringCapacity);
        const size_t expectedSize = wcslen(expected);
        if (size != expectedSize || size > 63)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<const wchar_t*>(nativeName)
                : *reinterpret_cast<wchar_t* const*>(nativeName);
        return value && wmemcmp(value, expected, size) == 0;
    }

    ReplySnapshot ReadReply() const
    {
        ReplySnapshot observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(&replyWriteVersion_, 0, 0);
            if (before & 1)
                continue;
            observed.sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(&replySequence_, 0, 0));
            observed.profile_group = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyProfileGroup_, 0, 0));
            observed.opcode = static_cast<uint16_t>(
                InterlockedCompareExchange(&replyOpcode_, 0, 0));
            observed.error = static_cast<uint16_t>(
                InterlockedCompareExchange(&replyError_, 0, 0));
            observed.channel_id = static_cast<uint16_t>(
                InterlockedCompareExchange(&replyChannel_, 0, 0));
            observed.consumed = static_cast<uint8_t>(
                InterlockedCompareExchange(&replyConsumed_, 0, 0));
            const LONG after =
                InterlockedCompareExchange(&replyWriteVersion_, 0, 0);
            if (before == after && !(after & 1))
                return observed;
        }
        memset(&observed, 0, sizeof(observed));
        return observed;
    }

    static int __fastcall ObserveVsChannelReply(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        ChannelActions* actions = active_;
        if (!actions || !actions->vsChannelReplyDispatch_)
            return 0;
        return DispatchAndObserveReply(
            actions,
            actions->vsChannelReplyDispatch_,
            self,
            a,
            b,
            c,
            opcode,
            packet);
    }

    static int __fastcall ObserveLobbyChannelReply(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        ChannelActions* actions = active_;
        if (!actions || !actions->lobbyChannelReplyDispatch_)
            return 0;
        return DispatchAndObserveReply(
            actions,
            actions->lobbyChannelReplyDispatch_,
            self,
            a,
            b,
            c,
            opcode,
            packet);
    }

    static int DispatchAndObserveReply(
        ChannelActions* actions,
        ChannelReplyDispatchFn original,
        void* self,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        const int normalizedOpcode = opcode & 0xFFFF;
        const bool isJoinReply =
            normalizedOpcode == kJoinChannelReplyOpcode && packet;
        uint16_t error = UINT16_MAX;
        uint16_t channel = 0;
        if (isJoinReply) {
            __try {
                const uint8_t* bytes =
                    static_cast<const uint8_t*>(packet);
                channel = *reinterpret_cast<const uint16_t*>(
                    bytes + kReplyChannel);
                error = *reinterpret_cast<const uint16_t*>(
                    bytes + kReplyError);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                error = UINT16_MAX;
                channel = 0;
            }
        }

        // Let the shipped handler consume 0x09DF first. A success observation
        // is published only after its real 0x6F3D00 profile update returns.
        const int result = original(
            self,
            a,
            b,
            c,
            opcode,
            packet);
        if (!isJoinReply)
            return result;

        const NativeActions::OnboardingObservation selected =
            actions->native_
                ? actions->native_->OnboardingState()
                : NativeActions::OnboardingObservation{};
        const bool consumed =
            error == 0 &&
            channel != 0 &&
            selected.channel_id == channel;
        InterlockedIncrement(&actions->replyWriteVersion_);
        InterlockedExchange(
            &actions->replyOpcode_,
            static_cast<LONG>(kJoinChannelReplyOpcode));
        InterlockedExchange(
            &actions->replyError_,
            static_cast<LONG>(error));
        InterlockedExchange(
            &actions->replyChannel_,
            static_cast<LONG>(channel));
        InterlockedExchange(
            &actions->replyProfileGroup_,
            static_cast<LONG>(selected.channel_group));
        InterlockedExchange(
            &actions->replyConsumed_,
            consumed ? 1 : 0);
        InterlockedIncrement(&actions->replySequence_);
        InterlockedIncrement(&actions->replyWriteVersion_);
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_ ? reinterpret_cast<Fn>(api_->Symbol(mod_, name)) : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    void* vsChannelVtable_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    VsChannelClickFn vsChannelClick_ = nullptr;
    VsChannelOpenFn vsChannelOpen_ = nullptr;
    ChannelReplyDispatchFn vsChannelReplyDispatch_ = nullptr;
    void* lobbyUiVtable_ = nullptr;
    LobbyJoinChannelFn lobbyJoinChannel_ = nullptr;
    LobbyBattleEntryFn lobbyBattleEntry_ = nullptr;
    uint32_t battleEntryAttempts_=0;
    bool choiceArmed_=false,choiceFailed_=false,sceneReloadRequired_=false;
    void* choiceOwner_=nullptr;
    uint32_t choiceGroup_=0;uint16_t choiceChannel_=0;
    ChannelReplyDispatchFn lobbyChannelReplyDispatch_ = nullptr;
    bool bound_ = false;

    mutable volatile LONG lastResult_ = kUnbound;
    mutable volatile LONG lastTargetGroup_ = 0;
    mutable volatile LONG requestSent_ = 0;
    mutable volatile LONG requestTargetGroup_ = 0;
    mutable volatile LONG requestTargetChannel_ = 0;
    mutable volatile LONG requestTransportSequence_ = 0;
    mutable volatile LONG requestReplyBaseline_ = 0;
    mutable volatile LONG replyWriteVersion_ = 0;
    mutable volatile LONG replySequence_ = 0;
    mutable volatile LONG replyOpcode_ = 0;
    mutable volatile LONG replyError_ = 0;
    mutable volatile LONG replyChannel_ = 0;
    mutable volatile LONG replyProfileGroup_ = 0;
    mutable volatile LONG replyConsumed_ = 0;
    uint16_t targetChannel_ = 0;
    uint32_t requiredType_ = 0;
    uint16_t excludedChannels_[64]{};
    uint32_t excludedCount_ = 0;
    uint32_t targetRow_ = 0;
    uint32_t targetPage_ = 0;
    uint32_t currentPage_ = 0;
    uint32_t maxPage_ = 0;
    inline static ChannelActions* active_ = nullptr;
};

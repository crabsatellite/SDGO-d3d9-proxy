#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "native_actions.h"

// Sends the shipped client's ordinary in-game mail request used by the
// CServicePaperSend UI. The server-side gift command remains authoritative:
// this adapter only proves that the native 0x0891 mail request was constructed
// and accepted by the original network transport. Command processing, its
// result mail, and the recipient's refreshed inventory are separate GameTest
// evidence gates.
class GiftActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kRequestSent = 2,

        kInvalidRecipient = 0x100,
        kInvalidPage = 0x101,
        kInvalidPosition = 0x102,
        kProfileUnavailable = 0x103,
        kSenderNicknameInvalid = 0x104,
        kPacketConstructionFailed = 0x105,
        kTransportRejected = 0x106,
        kNativeFault = 0x107,
        kInvalidBody = 0x108,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t profile_pilot_id;
        uint32_t page;
        uint32_t position;
        uint32_t packet_size;
        uint16_t opcode;
        uint8_t request_sent;
        uint8_t reserved;
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod, NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        profilePointer_ = Symbol<uint8_t**>("flow.profile_pointer");
        mailPacketCtor_ =
            Symbol<MailPacketCtorFn>("flow.mail_send_packet_ctor");
        bound_ =
            api_ && mod_ && native_ && profilePointer_ && mailPacketCtor_;
        Reset();
        return bound_;
    }

    void Reset()
    {
        memset(&observation_, 0, sizeof(observation_));
    }

    // Sends the ordinary player-to-player command:
    //     recipient = the target pilot nickname
    //     body      = "!give <page> <position>"
    //
    // The shipped packet owns 16 bytes for each nickname and 300 bytes for the
    // body. This method accepts byte strings because that is the exact ABI of
    // the original request constructor after the UI's code-page conversion.
    Result SendUnitGift(
        const char* recipient,
        uint32_t page,
        uint32_t position)
    {
        if (!bound_ || !native_)
            return Publish(kUnbound, page, position);

        const size_t recipientLength =
            BoundedLength(recipient, kNicknameCapacity);
        if (recipientLength == 0 ||
            recipientLength > kNicknameCapacity)
            return Publish(kInvalidRecipient, page, position);
        if (page == 0 || page > static_cast<uint32_t>(INT32_MAX))
            return Publish(kInvalidPage, page, position);
        if (position == 0 || position > kPageSize)
            return Publish(kInvalidPosition, page, position);

        char body[48]{};
        const int count=std::snprintf(body,sizeof(body),"!give %u %u",page,position);
        if(count<=0 || count>=static_cast<int>(sizeof(body)))return Publish(kInvalidBody,page,position);
        return SendMail(recipient,body,page,position);
    }

    // Ordinary authored mail commands are useful for a server's existing GM
    // preparation business. This never grants anything or bypasses permission;
    // the authenticated native sender and the server remain authoritative.
    Result SendMail(const char* recipient,const char* body,uint32_t page=0,uint32_t position=0)
    {
        if(!bound_ || !native_)return Publish(kUnbound,page,position);
        const auto recipientLength=BoundedLength(recipient,kNicknameCapacity);
        if(!recipientLength || recipientLength>kNicknameCapacity)return Publish(kInvalidRecipient,page,position);
        const auto bodyLength=BoundedLength(body,kBodyCapacity);
        if(!bodyLength || bodyLength>kBodyCapacity)return Publish(kInvalidBody,page,position);

        const NativeActions::OnboardingObservation nativeState =
            native_->OnboardingState();
        if (nativeState.native_app_state != kLobbyAppState)
            return Publish(
                kWaitingForLobby,
                page,
                position,
                nativeState);

        __try {
            uint8_t* profile =
                profilePointer_ ? *profilePointer_ : nullptr;
            if (!profile || nativeState.profile_pilot_id == 0)
                return Publish(
                    kProfileUnavailable,
                    page,
                    position,
                    nativeState);

            const char* sender = reinterpret_cast<const char*>(
                profile + kProfilePilotNickname);
            if (sender[0] == '\0')
                return Publish(
                    kSenderNicknameInvalid,
                    page,
                    position,
                    nativeState);

            alignas(4) uint8_t packet[kPacketSize] = {};
            void* constructed = mailPacketCtor_(
                packet,
                0,
                sender,
                0,
                recipient,
                body);
            if (constructed != packet ||
                !PacketMatches(
                    packet,
                    sender,
                    recipient,
                    recipientLength,
                    body,
                    static_cast<size_t>(bodyLength)))
                return Publish(
                    kPacketConstructionFailed,
                    page,
                    position,
                    nativeState);

            if (!native_->SendRequest(packet, sizeof(packet)))
                return Publish(
                    kTransportRejected,
                    page,
                    position,
                    nativeState);

            return Publish(
                kRequestSent,
                page,
                position,
                nativeState,
                true);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(
                kNativeFault,
                page,
                position,
                nativeState);
        }
    }

    Observation State() const { return observation_; }

private:
    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr uint32_t kPageSize = 6;
    static constexpr size_t kPacketSize = 0x159;
    static constexpr size_t kPacketSender = 0x06;
    static constexpr size_t kPacketRecipient = 0x1B;
    static constexpr size_t kPacketBody = 0x2C;
    static constexpr size_t kNicknameCapacity = 16;
    static constexpr size_t kBodyCapacity = 300;
    static constexpr size_t kProfilePilotNickname = 0x161;
    static constexpr uint16_t kMailRequestOpcode = 0x0891;

    using MailPacketCtorFn =
        void*(__thiscall*)(
            void*,
            uint32_t,
            const char*,
            uint32_t,
            const char*,
            const char*);

    static size_t BoundedLength(const char* value, size_t maximum)
    {
        if (!value)
            return maximum + 1;
        for (size_t index = 0; index <= maximum; ++index) {
            if (value[index] == '\0')
                return index;
        }
        return maximum + 1;
    }

    static bool PacketMatches(
        const uint8_t* packet,
        const char* sender,
        const char* recipient,
        size_t recipientLength,
        const char* body,
        size_t bodyLength)
    {
        if (!packet || !sender || !recipient || !body)
            return false;
        if (*reinterpret_cast<const uint16_t*>(packet) !=
                kMailRequestOpcode ||
            *reinterpret_cast<const uint32_t*>(packet + 0x02) != 0 ||
            *reinterpret_cast<const uint32_t*>(packet + 0x17) != 0)
            return false;

        const size_t senderLength =
            BoundedLength(sender, kNicknameCapacity);
        if (senderLength == 0 ||
            senderLength > kNicknameCapacity)
            return false;
        if (memcmp(packet + kPacketSender, sender, senderLength) != 0 ||
            memcmp(
                packet + kPacketRecipient,
                recipient,
                recipientLength) != 0 ||
            memcmp(packet + kPacketBody, body, bodyLength) != 0)
            return false;

        // The original constructor zeroes all three fixed buffers before its
        // bounded copies. Inputs shorter than their capacity must therefore
        // retain a terminator in the serialized request.
        if (senderLength < kNicknameCapacity &&
            packet[kPacketSender + senderLength] != 0)
            return false;
        if (recipientLength < kNicknameCapacity &&
            packet[kPacketRecipient + recipientLength] != 0)
            return false;
        if (bodyLength < kBodyCapacity &&
            packet[kPacketBody + bodyLength] != 0)
            return false;
        return true;
    }

    Result Publish(
        Result result,
        uint32_t page,
        uint32_t position,
        const NativeActions::OnboardingObservation& nativeState = {},
        bool requestSent = false)
    {
        observation_.result = result;
        observation_.native_app_state = nativeState.native_app_state;
        observation_.profile_pilot_id = nativeState.profile_pilot_id;
        observation_.page = page;
        observation_.position = position;
        observation_.packet_size =
            requestSent ? static_cast<uint32_t>(kPacketSize) : 0;
        observation_.opcode =
            requestSent ? kMailRequestOpcode : 0;
        observation_.request_sent = requestSent ? 1 : 0;
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_
            ? reinterpret_cast<Fn>(api_->Symbol(mod_, name))
            : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    MailPacketCtorFn mailPacketCtor_ = nullptr;
    Observation observation_ = {};
    bool bound_ = false;
};

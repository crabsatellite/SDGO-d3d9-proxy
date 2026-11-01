#pragma once

#include <windows.h>

#include <cstdint>

#include "native_actions.h"

// Drives the shipped CTutorialMode exit business method on the live mode
// object observed during its normal update. The method is the same virtual
// operation reached by the original PracticeEndBtn branch; it performs the
// native tutorial cleanup and scene transition without writing application,
// profile, UnitManager, or dialog-command state.
class TutorialActions
{
public:
    enum Result : uint32_t
    {
        kNone = 0,
        kModeObserved = 1,
        kExitInvoked = 2,
        kLeftGame = 3,
        kNativeFault = 4,
    };

    struct Observation
    {
        uint32_t update_sequence;
        uint32_t exit_request_sequence;
        uint32_t exit_return_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t tutorial_level;
        uint32_t profile_pilot_id;
        uint32_t native_thread_id;
        uint8_t native_return;
        uint8_t reserved[3];
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        tutorialUpdateEntry_ =
            Symbol<TutorialUpdateFn>("flow.tutorial_mode_update");
        tutorialUpdate_ = tutorialUpdateEntry_;
        tutorialExit_ =
            Symbol<TutorialExitFn>("flow.tutorial_mode_exit");
        tutorialVtable_ =
            Symbol<void**>("flow.tutorial_mode_vtable");

        if (!api_ || !mod_ || !native_ || !tutorialUpdate_ ||
            !tutorialExit_ || !tutorialVtable_ || !ValidVtable())
            return false;
        if (active_ && active_ != this)
            return false;

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.tutorial_mode_update",
                reinterpret_cast<void*>(&ObserveTutorialUpdate),
                reinterpret_cast<void**>(&tutorialUpdate_))) {
            active_ = nullptr;
            return false;
        }
        return true;
    }

    // Calls CTutorialMode's genuine vslot +0x84 operation on the exact object
    // captured from CTutorialMode::Update. PracticeEndBtn reaches this method
    // after the dialog closes; invoking it directly is the native business
    // action and does not fabricate a UI event or completion reply.
    bool ExitBeginnerTrial()
    {
        if (!native_ || native_->AppState() != kGameMainState)
            return false;

        const NativeActions::OnboardingObservation state =
            native_->OnboardingState();
        if (!state.profile_pilot_id || state.tutorial_level >= 2)
            return false;

        void* mode =
            InterlockedCompareExchangePointer(&tutorialMode_, nullptr, nullptr);
        const DWORD observedThread = static_cast<DWORD>(
            InterlockedCompareExchange(&nativeThreadId_, 0, 0));
        if (!mode || !observedThread ||
            observedThread != GetCurrentThreadId() ||
            !ValidMode(mode) ||
            InterlockedCompareExchange(&exitRequested_, 1, 0) != 0)
            return false;

        InterlockedIncrement(&exitRequestSequence_);
        InterlockedExchangePointer(&tutorialMode_, nullptr);
        __try {
            const uint8_t result = tutorialExit_(mode);
            InterlockedExchange(&nativeReturn_, result ? 1 : 0);
            InterlockedIncrement(&exitReturnSequence_);
            return result != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return false;
        }
    }

    Observation State() const
    {
        Observation observed = {};
        observed.update_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(&updateSequence_, 0, 0));
        observed.exit_request_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(&exitRequestSequence_, 0, 0));
        observed.exit_return_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(&exitReturnSequence_, 0, 0));
        observed.native_thread_id = static_cast<uint32_t>(
            InterlockedCompareExchange(&nativeThreadId_, 0, 0));
        observed.native_return = static_cast<uint8_t>(
            InterlockedCompareExchange(&nativeReturn_, 0, 0) != 0);
        observed.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        if (native_) {
            const NativeActions::OnboardingObservation state =
                native_->OnboardingState();
            observed.tutorial_level = state.tutorial_level;
            observed.profile_pilot_id = state.profile_pilot_id;
        }

        if (InterlockedCompareExchange(&nativeFault_, 0, 0) != 0) {
            observed.result = kNativeFault;
        } else if (observed.exit_return_sequence &&
                   observed.native_return &&
                   observed.native_app_state != kGameMainState) {
            observed.result = kLeftGame;
        } else if (observed.exit_return_sequence &&
                   observed.native_return) {
            observed.result = kExitInvoked;
        } else if (observed.update_sequence) {
            observed.result = kModeObserved;
        }
        return observed;
    }

private:
    static constexpr uint32_t kGameMainState = 22;
    static constexpr size_t kUpdateVtableIndex = 0x80 / sizeof(void*);
    static constexpr size_t kExitVtableIndex = 0x84 / sizeof(void*);

    using TutorialUpdateFn = uint8_t(__thiscall*)(void*);
    using TutorialExitFn = uint8_t(__thiscall*)(void*);

    bool ValidVtable() const
    {
        __try {
            return tutorialVtable_[kUpdateVtableIndex] ==
                       reinterpret_cast<void*>(tutorialUpdateEntry_) &&
                   tutorialVtable_[kExitVtableIndex] ==
                       reinterpret_cast<void*>(tutorialExit_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ValidMode(void* mode) const
    {
        if (!mode || !ValidVtable())
            return false;
        __try {
            return *reinterpret_cast<void***>(mode) == tutorialVtable_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static uint8_t __fastcall ObserveTutorialUpdate(void* self, void*)
    {
        TutorialActions* actions = active_;
        if (!actions || !actions->tutorialUpdate_)
            return 1;

        const bool valid = actions->ValidMode(self);
        const uint8_t result = actions->tutorialUpdate_(self);
        if (valid && result && actions->native_ &&
            actions->native_->AppState() == kGameMainState) {
            InterlockedExchangePointer(&actions->tutorialMode_, self);
            InterlockedExchange(
                &actions->nativeThreadId_,
                static_cast<LONG>(GetCurrentThreadId()));
            InterlockedIncrement(&actions->updateSequence_);
        } else if (InterlockedCompareExchangePointer(
                       &actions->tutorialMode_, nullptr, nullptr) == self) {
            InterlockedExchangePointer(&actions->tutorialMode_, nullptr);
        }
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
    TutorialUpdateFn tutorialUpdateEntry_ = nullptr;
    TutorialUpdateFn tutorialUpdate_ = nullptr;
    TutorialExitFn tutorialExit_ = nullptr;
    void** tutorialVtable_ = nullptr;
    mutable PVOID volatile tutorialMode_ = nullptr;
    mutable volatile LONG nativeThreadId_ = 0;
    mutable volatile LONG updateSequence_ = 0;
    mutable volatile LONG exitRequestSequence_ = 0;
    mutable volatile LONG exitReturnSequence_ = 0;
    mutable volatile LONG exitRequested_ = 0;
    mutable volatile LONG nativeReturn_ = 0;
    mutable volatile LONG nativeFault_ = 0;
    inline static TutorialActions* active_ = nullptr;
};

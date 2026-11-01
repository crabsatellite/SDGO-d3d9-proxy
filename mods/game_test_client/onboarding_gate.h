#pragma once

#include <cstdint>

namespace sdgo_game_test
{
    struct OnboardingCompletionEvidence
    {
        bool nickname_requested;
        bool nickname_accepted;
        uint32_t tutorial_exit_request_sequence;
        uint32_t tutorial_result;
        bool starter_request_attempted;
        uint32_t starter_result;
        uint32_t starter_result_confirm_sequence;
        uint32_t initial_loading_confirm_sequence;
        uint32_t initial_loading_result;
        uint32_t tutorial_level;
        bool starter_draw_busy;
        bool inventory_initialized;
        bool inventory_scan_complete;
        uint32_t inventory_owned_unit_count;
        uint32_t inventory_first_unit_id;
        uint32_t native_app_state;
        uint32_t authenticated_pilot;
        uint32_t current_pilot;
    };

    // These numeric values are part of the small GameTest-side observation
    // contract. They deliberately do not depend on the native action classes,
    // which keeps this completion rule independently testable.
    constexpr uint32_t kTutorialLeftGame = 3;
    constexpr uint32_t kStarterAccepted = 2;
    constexpr uint32_t kInitialLoadingAccepted = 2;

    inline bool IsNormalLobbyState(uint32_t appState)
    {
        return appState == 5 || appState == 6 || appState == 7 ||
               appState == 18 || appState == 20;
    }

    inline bool IsOnboardingComplete(
        const OnboardingCompletionEvidence& evidence)
    {
        const bool nicknameComplete =
            !evidence.nickname_requested || evidence.nickname_accepted;
        const bool tutorialExitComplete =
            evidence.tutorial_exit_request_sequence == 0 ||
            evidence.tutorial_result == kTutorialLeftGame;
        const bool starterComplete =
            !evidence.starter_request_attempted ||
            (evidence.starter_result == kStarterAccepted &&
             evidence.starter_result_confirm_sequence != 0);
        const bool initialLoadingComplete =
            evidence.initial_loading_confirm_sequence == 0 ||
            evidence.initial_loading_result == kInitialLoadingAccepted;
        const bool inventoryComplete =
            evidence.inventory_initialized &&
            evidence.inventory_scan_complete &&
            evidence.inventory_owned_unit_count != 0 &&
            evidence.inventory_first_unit_id != 0;
        const bool identityComplete =
            evidence.authenticated_pilot != 0 &&
            evidence.current_pilot == evidence.authenticated_pilot;

        return nicknameComplete &&
               tutorialExitComplete &&
               starterComplete &&
               initialLoadingComplete &&
               evidence.tutorial_level >= 2 &&
               !evidence.starter_draw_busy &&
               inventoryComplete &&
               IsNormalLobbyState(evidence.native_app_state) &&
               identityComplete;
    }
}

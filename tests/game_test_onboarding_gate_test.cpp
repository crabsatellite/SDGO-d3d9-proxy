#include <cstdlib>
#include <iostream>

#include "../mods/game_test_client/onboarding_gate.h"

namespace
{
    using sdgo_game_test::OnboardingCompletionEvidence;

    unsigned checks = 0;
    unsigned failures = 0;

    void Check(bool condition, const char* message)
    {
        ++checks;
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }

    OnboardingCompletionEvidence ExistingAccount()
    {
        OnboardingCompletionEvidence evidence{};
        evidence.tutorial_level = 2;
        evidence.inventory_initialized = true;
        evidence.inventory_scan_complete = true;
        evidence.inventory_owned_unit_count = 1;
        evidence.inventory_first_unit_id = 10003;
        evidence.native_app_state = 6;
        evidence.authenticated_pilot = 9229;
        evidence.current_pilot = 9229;
        return evidence;
    }

    OnboardingCompletionEvidence FreshAccount()
    {
        OnboardingCompletionEvidence evidence = ExistingAccount();
        evidence.nickname_requested = true;
        evidence.nickname_accepted = true;
        evidence.tutorial_exit_request_sequence = 1;
        evidence.tutorial_result = sdgo_game_test::kTutorialLeftGame;
        evidence.starter_request_attempted = true;
        evidence.starter_result = sdgo_game_test::kStarterAccepted;
        evidence.starter_result_confirm_sequence = 1;
        evidence.initial_loading_confirm_sequence = 1;
        evidence.initial_loading_result =
            sdgo_game_test::kInitialLoadingAccepted;
        return evidence;
    }
}

int main()
{
    Check(
        sdgo_game_test::IsOnboardingComplete(ExistingAccount()),
        "an already-initialized real account may skip one-time actions");
    Check(
        sdgo_game_test::IsOnboardingComplete(FreshAccount()),
        "a fresh account passes after every attempted native action completes");

    {
        auto evidence = FreshAccount();
        evidence.nickname_accepted = false;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "a sent nickname request requires its accepted reply");
    }
    {
        auto evidence = FreshAccount();
        evidence.tutorial_result = 2;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "an invoked tutorial exit must complete its scene transition");
    }
    {
        auto evidence = FreshAccount();
        evidence.starter_result = 0;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "a starter request requires a real accepted reply");
    }
    {
        auto evidence = FreshAccount();
        evidence.starter_result_confirm_sequence = 0;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "a fresh starter result must be confirmed through its native popup");
    }
    {
        auto evidence = FreshAccount();
        evidence.initial_loading_result = 0;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "an invoked initial-loading confirmation requires its accepted reply");
    }
    {
        auto evidence = ExistingAccount();
        evidence.inventory_scan_complete = false;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "partial inventory observation cannot complete onboarding");
    }
    {
        auto evidence = ExistingAccount();
        evidence.current_pilot = 9230;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "completion must remain bound to the authenticated pilot");
    }
    {
        auto evidence = ExistingAccount();
        evidence.native_app_state = 22;
        Check(
            !sdgo_game_test::IsOnboardingComplete(evidence),
            "game-main is not a completed onboarding lobby state");
    }

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

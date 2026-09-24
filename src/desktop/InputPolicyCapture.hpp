#pragma once

namespace Desktop {
    struct SInputPolicyCaptureDecision {
        bool cancelHeldButtons  = false;
        bool forcePolicyRefocus = false;
    };

    constexpr SInputPolicyCaptureDecision decideInputPolicyCapture(bool policyWindowOwnsPointer, bool buttonsHeld, bool dndActive) {
        if (dndActive)
            return {};

        if (!buttonsHeld)
            return {.forcePolicyRefocus = true};

        if (policyWindowOwnsPointer)
            return {.cancelHeldButtons = true, .forcePolicyRefocus = true};

        return {};
    }
}

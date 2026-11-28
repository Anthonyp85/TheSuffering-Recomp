// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone: clang++ -std=c++17 -Isrc tests/gameplay_input_regression.cpp -o gameplay_input_regression
#include "common/mkb_input.h"
#include "game/gameplay_input_state.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void ClothConservesElapsedTime() {
    for (int hz : {90, 120, 165}) {
        suffering::gameplay_input::ClothAccumulator cloth;
        const float dt = 1.0f / float(hz);
        double simulated = 0.0;
        bool fixed_steps = true;
        bool shared_frame = true;
        for (int frame = 0; frame < hz * 10; ++frame) {
            const float step = cloth.StepForFrame(uint32_t(frame), dt);
            simulated += step;
            if (step != 0.0f && std::fabs(step - 1.0f / 60.0f) > 1.0e-8f) fixed_steps = false;
            const double remainder = cloth.accum;
            for (int object = 0; object < 4; ++object) {
                if (cloth.StepForFrame(uint32_t(frame), dt) != step || cloth.accum != remainder) shared_frame = false;
            }
        }
        std::printf("cloth %d Hz: simulated=%.9f remainder=%.9f\n", hz, simulated, double(cloth.accum));
        Check(fixed_steps, "high-rate cloth always receives a single 60 Hz step");
        Check(shared_frame, "all cloth objects share a frame decision without advancing time");
        Check(std::fabs(simulated + cloth.accum - double(dt) * hz * 10) < 1.0e-10,
              "cloth simulated time plus remainder equals elapsed guest time");
        Check(cloth.accum >= 0.0 && cloth.accum < 1.0 / 60.0, "cloth remainder stays below one fixed step");
    }
    suffering::gameplay_input::ClothAccumulator cloth;
    Check(cloth.StepForFrame(0, 1.0f / 120.0f) == 0.0f, "first half-step waits");
    Check(cloth.StepForFrame(1, 1.0f / 30.0f) == 1.0f / 30.0f, "30 Hz cloth passes through");
    Check(cloth.accum == 0.0, "low-rate transition discards previous partial fixed step");
    Check(cloth.StepForFrame(2, 1.0f / 120.0f) == 0.0f, "high-rate transition starts a new accumulator");
    Check(std::isnan(cloth.StepForFrame(3, std::numeric_limits<float>::quiet_NaN())),
          "invalid guest dt retains the original pass-through contract");
}

void SliderRoundTripDoesNotSaveStaleValue() {
    using namespace suffering::gameplay_input;
    suffering::guest_settings::GuestUserSettings current{}, saved{};
    current.has_saved_values = saved.has_saved_values = true;
    auto snapshot = current;
    bool pending = false;
    Check(!CommitMenuSettings(snapshot, current, saved, true, pending), "unchanged drag does not save");
    snapshot.effects_volume = 0.5f;
    Check(!CommitMenuSettings(snapshot, current, saved, true, pending), "drag defers persistence");
    Check(pending && current.effects_volume == 0.5f && saved.effects_volume == 1.0f,
          "drag updates current snapshot but preserves saved baseline");
    snapshot.effects_volume = 1.0f;
    Check(!CommitMenuSettings(snapshot, current, saved, true, pending), "return drag still defers persistence");
    Check(current.effects_volume == 1.0f, "drag back to saved value updates current snapshot");
    Check(!FlushMenuSettings(current, saved), "release after round-trip drag does not save stale midpoint");
    Check(saved.effects_volume == 1.0f, "round-trip leaves persisted value unchanged");

    pending = false;
    snapshot.effects_volume = 0.75f;
    Check(!CommitMenuSettings(snapshot, current, saved, true, pending), "new drag remains deferred");
    Check(pending && FlushMenuSettings(current, saved), "changed drag saves on release");
    Check(saved.effects_volume == 0.75f && !FlushMenuSettings(current, saved), "deferred save is performed once");
    snapshot.effects_volume = 0.25f;
    Check(CommitMenuSettings(snapshot, current, saved, false, pending), "non-drag setting saves immediately");
    Check(!pending && saved.effects_volume == 0.25f, "immediate save updates persisted baseline");
}

void ReservedHostKeysCannotBeBound() {
    for (uint16_t vk : {0x70, 0x71, 0x72, 0x79, 0x5B, 0x5C}) {
        Check(!suffering::MkbCanBindKey(vk), "host shortcuts cannot become gameplay bindings");
    }
    Check(!suffering::MkbCanBindKey(0), "unmapped SDL scancode cannot become a binding");
    for (uint16_t vk : {0x41, 0x57, 0x73, 0x74, 0x7B, 0xA0}) {
        Check(suffering::MkbCanBindKey(vk), "ordinary keys and unreserved F keys remain bindable");
    }
}
}  // namespace

int main() {
    ClothConservesElapsedTime();
    SliderRoundTripDoesNotSaveStaleValue();
    ReservedHostKeysCannotBeBound();
    std::printf("gameplay/input regression: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace controller {

constexpr uint8_t computeGroup(uint8_t active_buttons) {
    return static_cast<uint8_t>(active_buttons & 0x7f);
}

struct AxisCalibration {
    uint16_t minimum;
    uint16_t center;
    uint16_t maximum;
    uint16_t center_deadzone;
};

constexpr bool isValidCalibration(const AxisCalibration &calibration, uint16_t minimum_span) {
    return calibration.minimum < calibration.center &&
           calibration.center < calibration.maximum &&
           calibration.maximum - calibration.minimum >= minimum_span &&
           calibration.center - calibration.minimum > calibration.center_deadzone &&
           calibration.maximum - calibration.center > calibration.center_deadzone;
}

constexpr int16_t normalizeAxis(uint32_t raw, const AxisCalibration &calibration) {
    if (raw <= calibration.minimum) return INT16_MIN;
    if (raw >= calibration.maximum) return INT16_MAX;
    const uint32_t negative_edge = calibration.center - calibration.center_deadzone;
    const uint32_t positive_edge = calibration.center + calibration.center_deadzone;
    if (raw >= negative_edge && raw <= positive_edge) return 0;
    if (raw < negative_edge) {
        const int32_t span = static_cast<int32_t>(negative_edge - calibration.minimum);
        return static_cast<int16_t>(INT16_MIN +
            static_cast<int32_t>((raw - calibration.minimum) * 32768u / span));
    }
    const uint32_t span = calibration.maximum - positive_edge;
    return static_cast<int16_t>((raw - positive_edge) * 32767u / span);
}

class Debouncer {
public:
    Debouncer(bool initial = false, uint8_t required_samples = 2)
        : stable_(initial), candidate_(initial), required_samples_(std::max<uint8_t>(required_samples, 1)), count_(0) {}

    bool update(bool sample) {
        if (sample == stable_) {
            candidate_ = sample;
            count_ = 0;
            return stable_;
        }
        if (sample != candidate_) {
            candidate_ = sample;
            count_ = 1;
        } else if (count_ < required_samples_) {
            ++count_;
        }
        if (count_ >= required_samples_) {
            stable_ = candidate_;
            count_ = 0;
        }
        return stable_;
    }

    [[nodiscard]] bool value() const { return stable_; }

private:
    bool stable_;
    bool candidate_;
    uint8_t required_samples_;
    uint8_t count_;
};

enum class SelectorPosition : uint8_t { Safe, Emergency, Armed, Invalid };

struct SafetyState {
    bool deadman = false;
    bool emergency_stop = false;
    bool startup_inhibited = true;
    SelectorPosition position = SelectorPosition::Safe;
};

class SafetyInterlock {
public:
    explicit SafetyInterlock(uint8_t required_samples = 2)
        : required_samples_(std::max<uint8_t>(required_samples, 1)) {}

    SafetyState update(bool lower_active, bool upper_active) {
        const SelectorPosition raw = decode(lower_active, upper_active);

        if (raw == SelectorPosition::Emergency || raw == SelectorPosition::Invalid) {
            stable_ = raw;
            candidate_ = raw;
            count_ = 0;
        } else if (raw != candidate_) {
            candidate_ = raw;
            count_ = 1;
        } else if (raw != stable_ && count_ < required_samples_) {
            ++count_;
            if (count_ >= required_samples_) {
                stable_ = candidate_;
                count_ = 0;
            }
        }

        if (raw == SelectorPosition::Safe && stable_ == SelectorPosition::Safe) {
            if (safe_samples_ < required_samples_) ++safe_samples_;
            if (safe_samples_ >= required_samples_) startup_inhibited_ = false;
        } else {
            safe_samples_ = 0;
        }

        const bool emergency = raw == SelectorPosition::Emergency || raw == SelectorPosition::Invalid ||
                               stable_ == SelectorPosition::Emergency || stable_ == SelectorPosition::Invalid;
        const bool deadman = !startup_inhibited_ && !emergency && raw == SelectorPosition::Armed &&
                             stable_ == SelectorPosition::Armed;
        return SafetyState{deadman, emergency, startup_inhibited_, emergency ? raw : stable_};
    }

private:
    static constexpr SelectorPosition decode(bool lower, bool upper) {
        if (lower && upper) return SelectorPosition::Invalid;
        if (lower) return SelectorPosition::Emergency;
        if (upper) return SelectorPosition::Armed;
        return SelectorPosition::Safe;
    }

    uint8_t required_samples_;
    uint8_t count_ = 0;
    uint8_t safe_samples_ = 0;
    SelectorPosition stable_ = SelectorPosition::Safe;
    SelectorPosition candidate_ = SelectorPosition::Safe;
    bool startup_inhibited_ = true;
};

struct ControllerSnapshot {
    std::array<int16_t, 4> axes{};
    uint16_t switches = 0;
    SafetyState safety{};
};

inline bool hasMeaningfulInputChange(const ControllerSnapshot &previous,
                                     const ControllerSnapshot &current,
                                     uint16_t axis_threshold) {
    for (size_t index = 0; index < previous.axes.size(); ++index) {
        const int32_t difference = static_cast<int32_t>(current.axes[index]) - previous.axes[index];
        if (difference >= axis_threshold || difference <= -static_cast<int32_t>(axis_threshold)) return true;
    }
    return previous.switches != current.switches ||
           previous.safety.deadman != current.safety.deadman ||
           previous.safety.emergency_stop != current.safety.emergency_stop ||
           previous.safety.startup_inhibited != current.safety.startup_inhibited ||
           previous.safety.position != current.safety.position;
}

} // namespace controller

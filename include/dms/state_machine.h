#pragma once

#include <string>
#include <deque>

namespace dms {

struct StateMachineResult {
    std::string status;    // "normal", "eyes_closing", "sleeping", "yawning", "no_face"
    std::string buzz;      // "", "short", "long"
    bool is_new_alert = false;
};

struct StateMachineStats {
    std::string state;
    int sleep_events_5min = 0;
    int yawn_events_5min = 0;
};

// Time-windowed drowsiness detection state machine
class DrowsinessStateMachine {
public:
    DrowsinessStateMachine();

    // Update with new detection frame
    StateMachineResult update(bool face_detected, float eye_closed_prob,
                               float yawn_prob, float speed);

    // Get rolling-window event stats
    StateMachineStats get_stats() const;

    const std::string& state() const { return state_; }

private:
    std::string state_;
    double eyes_closed_since_ = 0;
    double no_face_since_ = 0;
    double last_sleep_alert_ = 0;
    double last_yawn_alert_ = 0;

    std::deque<double> sleep_events_;
    std::deque<double> yawn_events_;

    // Config values (cached from global config)
    float eyes_closed_duration_;
    float sleep_cooldown_;
    float yawn_cooldown_;
    float no_face_timeout_;
    float eye_prob_threshold_;
    float yawn_prob_threshold_;
    float warning_window_;
};

} // namespace dms

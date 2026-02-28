#include <dms/state_machine.h>
#include <dms/config.h>
#include <chrono>
#include <algorithm>

namespace dms {

static double monotonic_now() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

DrowsinessStateMachine::DrowsinessStateMachine()
    : state_("normal")
{
    const auto& cfg = config();
    eyes_closed_duration_ = cfg.eyes_closed_duration;
    sleep_cooldown_ = cfg.sleep_alert_cooldown;
    yawn_cooldown_ = cfg.yawn_alert_cooldown;
    no_face_timeout_ = cfg.no_face_timeout;
    eye_prob_threshold_ = cfg.eye_closed_prob;
    yawn_prob_threshold_ = cfg.yawn_prob;
    warning_window_ = cfg.warning_window;
}

StateMachineResult DrowsinessStateMachine::update(
    bool face_detected, float eye_closed_prob, float yawn_prob, float speed)
{
    double now = monotonic_now();

    // --- No face ---
    if (!face_detected) {
        if (no_face_since_ == 0.0) {
            no_face_since_ = now;
        }
        eyes_closed_since_ = 0.0;
        double no_face_duration = now - no_face_since_;

        state_ = "no_face";

        if (speed > 0 && no_face_duration >= no_face_timeout_) {
            return {"no_face", "short", true};
        }
        return {"no_face", "", false};
    }

    // Face detected — reset no-face timer
    no_face_since_ = 0.0;

    std::string buzz;
    bool is_new = false;

    bool eyes_closed = eye_closed_prob > eye_prob_threshold_;
    bool yawning = yawn_prob > yawn_prob_threshold_;

    // --- Eye closure detection (duration-based) ---
    if (eyes_closed) {
        if (eyes_closed_since_ == 0.0) {
            eyes_closed_since_ = now;
        }
        double closed_duration = now - eyes_closed_since_;

        if (closed_duration >= eyes_closed_duration_) {
            state_ = "sleeping";
            if (now - last_sleep_alert_ > sleep_cooldown_) {
                last_sleep_alert_ = now;
                sleep_events_.push_back(now);
                if (sleep_events_.size() > 100) sleep_events_.pop_front();
                buzz = "long";
                is_new = true;
            }
            return {"sleeping", buzz, is_new};
        }

        state_ = "eyes_closing";
    } else {
        eyes_closed_since_ = 0.0;
    }

    // --- Yawn detection ---
    if (yawning) {
        state_ = "yawning";
        if (now - last_yawn_alert_ > yawn_cooldown_) {
            last_yawn_alert_ = now;
            yawn_events_.push_back(now);
            if (yawn_events_.size() > 100) yawn_events_.pop_front();
            buzz = "short";
            is_new = true;
        }
        return {"yawning", buzz, is_new};
    }

    // --- Normal ---
    if (!eyes_closed) {
        state_ = "normal";
    }

    return {state_, "", false};
}

StateMachineStats DrowsinessStateMachine::get_stats() const {
    double now = monotonic_now();
    double cutoff = now - warning_window_;

    int sleep_count = 0;
    for (auto t : sleep_events_) {
        if (t > cutoff) sleep_count++;
    }
    int yawn_count = 0;
    for (auto t : yawn_events_) {
        if (t > cutoff) yawn_count++;
    }

    return {state_, sleep_count, yawn_count};
}

} // namespace dms

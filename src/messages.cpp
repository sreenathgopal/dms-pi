#include <dms/messages.h>
#include <chrono>

namespace dms {

double now_timestamp() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    auto dur = tp.time_since_epoch();
    return duration_cast<duration<double>>(dur).count();
}

DetectionStatusMsg make_detection_status(
    const std::string& status, float ear_l, float ear_r, float mar,
    float speed, const std::string& buzz, const std::string& gaze)
{
    return DetectionStatusMsg{
        status,
        std::round(ear_l * 10000.0f) / 10000.0f,
        std::round(ear_r * 10000.0f) / 10000.0f,
        std::round(mar * 10000.0f) / 10000.0f,
        std::round(speed * 100.0f) / 100.0f,
        buzz,
        gaze,
        now_timestamp()
    };
}

DetectionImageMsg make_detection_image(
    const std::vector<uint8_t>& jpeg, const std::string& status)
{
    return DetectionImageMsg{jpeg, status, now_timestamp()};
}

GpsPositionMsg make_gps_position(float speed, double lat, double lon, float acc) {
    return GpsPositionMsg{
        std::round(speed * 100.0f) / 100.0f,
        std::round(lat * 1e6) / 1e6,
        std::round(lon * 1e6) / 1e6,
        std::round(acc * 100.0f) / 100.0f,
        now_timestamp()
    };
}

} // namespace dms

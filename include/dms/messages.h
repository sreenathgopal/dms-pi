#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <msgpack.hpp>

namespace dms {

// Detection status message (published on "detection.status")
struct DetectionStatusMsg {
    std::string status;
    float ear_l = 0;
    float ear_r = 0;
    float mar = 0;
    float speed = 0;
    std::string buzz;   // "", "short", "long"
    std::string gaze;   // "left", "center", "right", ""
    double ts = 0;

    MSGPACK_DEFINE_MAP(status, ear_l, ear_r, mar, speed, buzz, gaze, ts)
};

// Detection image message (published on "detection.image")
struct DetectionImageMsg {
    std::vector<uint8_t> jpeg;
    std::string status;
    double ts = 0;

    MSGPACK_DEFINE_MAP(jpeg, status, ts)
};

// GPS position message (published on "gps.position")
struct GpsPositionMsg {
    float speed = 0;
    double lat = 0;
    double lon = 0;
    float acc = 0;
    double ts = 0;

    MSGPACK_DEFINE_MAP(speed, lat, lon, acc, ts)
};

// Pack a message into a msgpack buffer
template<typename T>
std::string pack(const T& msg) {
    msgpack::sbuffer buf;
    msgpack::pack(buf, msg);
    return std::string(buf.data(), buf.size());
}

// Unpack a msgpack buffer into a message
template<typename T>
T unpack(const void* data, size_t size) {
    auto oh = msgpack::unpack(static_cast<const char*>(data), size);
    T msg;
    oh.get().convert(msg);
    return msg;
}

template<typename T>
T unpack(const std::string& data) {
    return unpack<T>(data.data(), data.size());
}

} // namespace dms

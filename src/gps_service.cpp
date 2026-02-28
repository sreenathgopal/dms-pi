#include <dms/gps_service.h>
#include <dms/config.h>
#include <dms/messages.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <sstream>

#ifndef DMS_NO_GPS
// POSIX serial
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#endif

namespace dms {

GpsPositionMsg make_gps_position(float speed, double lat, double lon, float acc);

#ifndef DMS_NO_GPS

// Parse NMEA RMC sentence: $GPRMC or $GNRMC
// Fields: $xxRMC,time,status,lat,N/S,lon,E/W,speed_knots,course,date,...
static bool parse_rmc(const std::string& line,
                      double& lat, double& lon, float& speed_kmh) {
    if (line.find("RMC") == std::string::npos) return false;

    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }

    if (fields.size() < 8) return false;
    if (fields[2] != "A") return false;  // 'A' = active, 'V' = void

    // Latitude: DDMM.MMMM
    if (!fields[3].empty()) {
        double raw = std::strtod(fields[3].c_str(), nullptr);
        int degrees = static_cast<int>(raw / 100);
        double minutes = raw - degrees * 100;
        lat = degrees + minutes / 60.0;
        if (fields[4] == "S") lat = -lat;
    }

    // Longitude: DDDMM.MMMM
    if (!fields[5].empty()) {
        double raw = std::strtod(fields[5].c_str(), nullptr);
        int degrees = static_cast<int>(raw / 100);
        double minutes = raw - degrees * 100;
        lon = degrees + minutes / 60.0;
        if (fields[6] == "W") lon = -lon;
    }

    // Speed in knots -> km/h
    if (!fields[7].empty()) {
        float knots = std::strtof(fields[7].c_str(), nullptr);
        speed_kmh = knots * 1.852f;
    } else {
        speed_kmh = 0.0f;
    }

    return true;
}

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B9600;
    }
}

#endif // DMS_NO_GPS

void gps_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    const auto& cfg = config();

    // --- ZeroMQ publisher ---
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.set(zmq::sockopt::sndhwm, 32);
    pub.set(zmq::sockopt::linger, 1000);
    pub.bind(cfg.zmq_gps_endpoint);

#ifdef DMS_NO_GPS
    std::cout << "[gps] Built without GPS — publishing zero speed" << std::endl;

    while (!shutdown.load()) {
        auto msg = make_gps_position(0.0f, 0.0, 0.0, 0.0f);
        auto packed = pack(msg);
        pub.send(zmq::buffer("gps.position"), zmq::send_flags::sndmore);
        pub.send(zmq::buffer(packed), zmq::send_flags::none);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    pub.close();
    std::cout << "[gps] GPS service stopped" << std::endl;

#else // Full GPS with POSIX serial

    // --- Open serial port ---
    int serial_fd = open(cfg.gps_port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        std::cerr << "[gps] Serial unavailable: " << cfg.gps_port
                  << " (" << strerror(errno) << ") — will publish zero speed" << std::endl;
    } else {
        // Configure serial port
        struct termios tty;
        std::memset(&tty, 0, sizeof(tty));
        tcgetattr(serial_fd, &tty);

        cfsetispeed(&tty, baud_to_speed(cfg.gps_baud));
        cfsetospeed(&tty, baud_to_speed(cfg.gps_baud));

        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_oflag &= ~OPOST;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 10;  // 1 second timeout

        tcsetattr(serial_fd, TCSANOW, &tty);
        std::cout << "[gps] Serial opened: " << cfg.gps_port
                  << " @ " << cfg.gps_baud << " baud" << std::endl;
    }

    float prev_speed_ms = 0.0f;
    auto prev_time = std::chrono::steady_clock::now();
    bool first_fix = true;

    std::cout << "[gps] GPS service running" << std::endl;

    // Line buffer for reading serial data
    std::string line_buf;
    char read_buf[256];

    while (!shutdown.load()) {
        if (serial_fd < 0) {
            // No GPS hardware — publish zero speed periodically
            auto msg = make_gps_position(0.0f, 0.0, 0.0, 0.0f);
            auto packed = pack(msg);
            pub.send(zmq::buffer("gps.position"), zmq::send_flags::sndmore);
            pub.send(zmq::buffer(packed), zmq::send_flags::none);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Read from serial
        ssize_t n = read(serial_fd, read_buf, sizeof(read_buf) - 1);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        read_buf[n] = '\0';

        // Accumulate into line buffer
        line_buf.append(read_buf, n);

        // Process complete lines
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);

            // Trim
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();

            if (line.empty() || line[0] != '$') continue;

            double lat = 0, lon = 0;
            float speed_kmh = 0;

            if (!parse_rmc(line, lat, lon, speed_kmh)) continue;

            // Calculate acceleration
            auto now = std::chrono::steady_clock::now();
            float acc = 0.0f;
            if (!first_fix) {
                double dt = std::chrono::duration<double>(now - prev_time).count();
                if (dt > 0.1) {
                    float current_speed_ms = speed_kmh / 3.6f;
                    acc = (current_speed_ms - prev_speed_ms) / static_cast<float>(dt);
                    prev_speed_ms = current_speed_ms;
                }
            }
            first_fix = false;
            prev_time = now;

            // Publish
            auto msg = make_gps_position(speed_kmh, lat, lon, acc);
            auto packed = pack(msg);
            pub.send(zmq::buffer("gps.position"), zmq::send_flags::sndmore);
            pub.send(zmq::buffer(packed), zmq::send_flags::none);
        }

        // Prevent line_buf from growing unbounded
        if (line_buf.size() > 1024) {
            line_buf.clear();
        }
    }

    // Cleanup
    if (serial_fd >= 0) {
        close(serial_fd);
    }
    pub.close();
    std::cout << "[gps] GPS service stopped" << std::endl;

#endif // DMS_NO_GPS
}

} // namespace dms

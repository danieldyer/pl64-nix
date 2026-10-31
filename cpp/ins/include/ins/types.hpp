#pragma once
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ins {
using Time = std::chrono::nanoseconds;
using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Mat15 = Eigen::Matrix<double, 15, 15>;
using Vec15 = Eigen::Matrix<double, 15, 1>;
using Quat = Eigen::Quaterniond;
inline double seconds(Time t) { return std::chrono::duration<double>(t).count(); }
inline Time time_from_seconds(double t) {
    return std::chrono::duration_cast<Time>(std::chrono::duration<double>(t));
}
inline constexpr double pi = 3.14159265358979323846;
inline constexpr double degrees = pi / 180.;
inline constexpr double standard_gravity = 9.80665;

struct Stamp {
    Time arrival{}, utc{}, measurement{};
    double uncertainty_s = .01;
    std::uint64_t sequence = 0;
};
struct ImuSample {
    Stamp stamp;
    Vec3 omega = Vec3::Zero(), force = Vec3::Zero();
    std::uint8_t counter = 0;
    bool gap = false, saturated = false;
};
struct GnssFix {
    Stamp stamp;
    double latitude = 0, longitude = 0, height = 0;
    Vec3 velocity = Vec3::Zero(); // NED at the antenna
    Vec3 position_sigma = Vec3::Constant(5), velocity_sigma = Vec3::Constant(.2);
    bool valid = false;
    unsigned satellites = 0;
};
// Quaternion storage follows Eigen (x,y,z,w), mapping body -> fixed origin NED.
// Contiguous SI-valued state storage is also used by the recording schema.
struct State {
    Time time{};
    std::array<double, 16> data{0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    Eigen::Map<Vec3> position() { return Eigen::Map<Vec3>(data.data()); }
    Eigen::Map<const Vec3> position() const { return Eigen::Map<const Vec3>(data.data()); }
    Eigen::Map<Vec3> velocity() { return Eigen::Map<Vec3>(data.data() + 3); }
    Eigen::Map<const Vec3> velocity() const { return Eigen::Map<const Vec3>(data.data() + 3); }
    Eigen::Map<Quat> attitude() { return Eigen::Map<Quat>(data.data() + 6); }
    Eigen::Map<const Quat> attitude() const { return Eigen::Map<const Quat>(data.data() + 6); }
    Eigen::Map<Vec3> gyro_bias() { return Eigen::Map<Vec3>(data.data() + 10); }
    Eigen::Map<const Vec3> gyro_bias() const { return Eigen::Map<const Vec3>(data.data() + 10); }
    Eigen::Map<Vec3> accel_bias() { return Eigen::Map<Vec3>(data.data() + 13); }
    Eigen::Map<const Vec3> accel_bias() const { return Eigen::Map<const Vec3>(data.data() + 13); }
};
enum class Stage { built_in_test, coarse_alignment, fine_alignment, navigating, degraded };
std::string_view stage_name(Stage);
struct Estimate {
    State state;
    Mat15 covariance = Mat15::Identity() * 1e6;
    Stage stage = Stage::built_in_test;
    bool heading_observed = false;
    double gnss_age_s = 0, update_ms = 0, normalized_residual = 0;
    std::uint64_t revision = 0;
    std::string reason;
};
struct Noise {
    Vec3 gyro_density = Vec3(.856, .592, .698) * degrees / 60.;
    Vec3 accel_density = Vec3(.0849, .0877, .0890) / 60.;
    // Provisional driving noise, not fitted by a five-minute Allan capture.
    Vec3 gyro_bias_walk = Vec3::Constant(.05 * degrees / 3600.);
    Vec3 accel_bias_walk = Vec3::Constant(2e-5);
};
struct Config {
    Noise noise;
    Vec3 gnss_lever = Vec3::Zero();
    Quat sensor_to_body = Quat::Identity();
    double imu_rate = 200., history_s = 3.;
    double imu_delay_s = .001, imu_timing_sigma_s = .01, gnss_timing_sigma_s = .1;
    double gyro_initial_sigma = .1 * degrees, accel_initial_sigma = .1;
    double gnss_position_floor = 1., gnss_velocity_floor = .08;
    double gnss_correlation_s = 2., gnss_correlated_position_sigma = 1.5;
    double navigation_heading_sigma = 3. * degrees;
    void validate() const;
};
enum class Source { imu, gnss, clock, estimator, simulator };
enum class Kind { raw, imu, gnss, state, health, metadata, truth };
struct Record {
    Kind kind = Kind::raw;
    Source source = Source::imu;
    Stamp stamp;
    std::uint64_t native_counter = 0;
    std::vector<std::uint8_t> payload;
    std::vector<double> values, covariance;
    std::uint32_t flags = 0;
    std::string detail;
    double gnss_age_s = 0, update_ms = 0, normalized_residual = 0;
};
} // namespace ins

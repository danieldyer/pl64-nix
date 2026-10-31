#include "ins/config.hpp"
#include <stdexcept>
namespace ins {
std::string_view stage_name(Stage s) {
    switch (s) {
    case Stage::built_in_test:
        return "built_in_test";
    case Stage::coarse_alignment:
        return "coarse_alignment";
    case Stage::fine_alignment:
        return "fine_alignment";
    case Stage::navigating:
        return "navigating";
    case Stage::degraded:
        return "degraded";
    }
    return "unknown";
}
void Config::validate() const {
    if (imu_rate != 200 || !std::isfinite(history_s) || history_s < 1 || history_s > 10)
        throw std::invalid_argument("Invalid IMU cadence or delayed-measurement history");
    const std::array positive{
        gyro_initial_sigma,  accel_initial_sigma,      gnss_position_floor,
        gnss_velocity_floor, navigation_heading_sigma, imu_timing_sigma_s,
        gnss_timing_sigma_s, gnss_correlation_s,       gnss_correlated_position_sigma};
    for (auto x : positive)
        if (!std::isfinite(x) || x <= 0)
            throw std::invalid_argument("Invalid uncertainty");
    if (!gnss_lever.allFinite() || gnss_lever.norm() > 100 ||
        !sensor_to_body.coeffs().allFinite() || std::abs(sensor_to_body.norm() - 1) > 1e-6)
        throw std::invalid_argument("Invalid mounting geometry");
    for (const auto *v :
         {&noise.gyro_density, &noise.accel_density, &noise.gyro_bias_walk, &noise.accel_bias_walk})
        if (!v->allFinite() || v->minCoeff() <= 0)
            throw std::invalid_argument("Invalid IMU noise density");
    if (!std::isfinite(imu_delay_s) || std::abs(imu_delay_s) > .5)
        throw std::invalid_argument("Invalid IMU latency");
}
Config configuration(const nlohmann::json &j) {
    Config c;
    const auto known = configuration_json(c);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.contains(it.key()))
            throw std::invalid_argument("Unknown navigation setting: " + it.key());
    auto vec = [&](const char *key, Vec3 &v) {
        if (j.contains(key)) {
            auto a = j.at(key).get<std::array<double, 3>>();
            v = Eigen::Map<Vec3>(a.data());
        }
    };
    vec("gnss_lever_m", c.gnss_lever);
    vec("gyro_noise_rad_sqrt_s", c.noise.gyro_density);
    vec("accel_noise_m_s_sqrt_s", c.noise.accel_density);
    vec("gyro_bias_walk", c.noise.gyro_bias_walk);
    vec("accel_bias_walk", c.noise.accel_bias_walk);
    if (j.contains("sensor_to_body_xyzw")) {
        auto q = j.at("sensor_to_body_xyzw").get<std::array<double, 4>>();
        c.sensor_to_body = Eigen::Map<Quat>(q.data());
    }
    c.imu_rate = j.value("imu_rate_hz", c.imu_rate);
    c.history_s = j.value("history_s", c.history_s);
    c.imu_delay_s = j.value("imu_delay_s", c.imu_delay_s);
    c.imu_timing_sigma_s = j.value("imu_timing_sigma_s", c.imu_timing_sigma_s);
    c.gnss_timing_sigma_s = j.value("gnss_timing_sigma_s", c.gnss_timing_sigma_s);
    c.gnss_position_floor = j.value("gnss_position_floor_m", c.gnss_position_floor);
    c.gnss_velocity_floor = j.value("gnss_velocity_floor_mps", c.gnss_velocity_floor);
    c.gnss_correlation_s = j.value("gnss_correlation_s", c.gnss_correlation_s);
    c.gnss_correlated_position_sigma =
        j.value("gnss_correlated_position_sigma_m", c.gnss_correlated_position_sigma);
    c.gyro_initial_sigma = j.value("gyro_initial_sigma_rad_s", c.gyro_initial_sigma);
    c.accel_initial_sigma = j.value("accel_initial_sigma_mps2", c.accel_initial_sigma);
    c.navigation_heading_sigma =
        j.value("navigation_heading_sigma_deg", c.navigation_heading_sigma / degrees) * degrees;
    c.validate();
    return c;
}
nlohmann::json configuration_json(const Config &c) {
    auto a = [](const auto &v) { return std::vector<double>(v.data(), v.data() + v.size()); };
    return {{"imu_rate_hz", c.imu_rate},
            {"history_s", c.history_s},
            {"gnss_lever_m", a(c.gnss_lever)},
            {"sensor_to_body_xyzw", a(c.sensor_to_body.coeffs())},
            {"imu_delay_s", c.imu_delay_s},
            {"imu_timing_sigma_s", c.imu_timing_sigma_s},
            {"gnss_timing_sigma_s", c.gnss_timing_sigma_s},
            {"gyro_noise_rad_sqrt_s", a(c.noise.gyro_density)},
            {"accel_noise_m_s_sqrt_s", a(c.noise.accel_density)},
            {"gyro_bias_walk", a(c.noise.gyro_bias_walk)},
            {"accel_bias_walk", a(c.noise.accel_bias_walk)},
            {"gnss_position_floor_m", c.gnss_position_floor},
            {"gnss_velocity_floor_mps", c.gnss_velocity_floor},
            {"gyro_initial_sigma_rad_s", c.gyro_initial_sigma},
            {"accel_initial_sigma_mps2", c.accel_initial_sigma},
            {"navigation_heading_sigma_deg", c.navigation_heading_sigma / degrees},
            {"gnss_correlation_s", c.gnss_correlation_s},
            {"gnss_correlated_position_sigma_m", c.gnss_correlated_position_sigma}};
}
} // namespace ins

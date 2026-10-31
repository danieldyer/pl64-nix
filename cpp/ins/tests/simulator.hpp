#pragma once
#include "ins/config.hpp"
#include "ins/model.hpp"
#include <functional>

namespace ins {
struct Simulation {
    std::string scenario = "maneuver";
    double stop_duration_s = 5;
    double duration_s = 60, start_delay_s = 5, heading = 125 * degrees, roll = 0, pitch = 0;
    double latitude = -38 * degrees, longitude = 145 * degrees, height = 50;
    std::uint64_t seed = 1;
    bool noise = true, startup_zero = false;
    Noise sensor_noise;
    Vec3 gyro_bias = Vec3(.006, -.036, -.035) * degrees;
    Vec3 accel_bias = Vec3(.015, -.02, .025);
    Vec3 gyro_scale_ppm = Vec3::Constant(100), accel_scale_ppm = Vec3::Constant(100);
    Vec3 gyro_temperature = Vec3::Constant(.0001 * degrees),
         accel_temperature = Vec3::Constant(.0002);
    double cross_axis = 1e-4, gyro_g_sensitivity = 1e-6;
    double gyro_bias_tau_s = 600, accel_bias_tau_s = 600;
    double sensor_bandwidth_hz = 100, clock_ppm = 20;
    double imu_latency_s = .001, imu_jitter_s = .0001;
    double gnss_latency_s = .12, gnss_jitter_s = .02;
    double gnss_position_sigma = 1.5, gnss_velocity_sigma = .08, gnss_correlation_s = 2;
    double gnss_white_position_sigma = 0;
    bool gnss_stationary_error_start = false;
    double dropout_start_s = -1, dropout_end_s = -1;
    unsigned corrupt_every = 0, drop_every = 0, duplicate_every = 0;
    Vec3 gnss_lever = Vec3(.3, .1, -.2);
};
struct Truth {
    State state;
    Vec3 acceleration = Vec3::Zero(), body_rate = Vec3::Zero();
};
Truth trajectory(const Simulation &, double time);
Simulation simulation_configuration(const nlohmann::json &);
nlohmann::json simulation_json(const Simulation &);
// Events are delivered in host arrival order. Truth is emitted separately and
// never passed to the estimator. Binary payloads include quantization/checksums.
void simulate(const Simulation &, const std::function<void(const Record &)> &);
} // namespace ins

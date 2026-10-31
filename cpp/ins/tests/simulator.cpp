#include "simulator.hpp"
#include "ins/protocol.hpp"
#include <algorithm>
#include <queue>
#include <random>
#include <stdexcept>

namespace ins {
Truth trajectory(const Simulation &s, double time) {
    Truth out;
    out.state.time = time_from_seconds(time);
    const double t = std::max(0., time - s.start_delay_s);
    double roll = s.roll, pitch = s.pitch, heading = s.heading, rd = 0, pd = 0, hd = 0;
    const bool moving = time >= s.start_delay_s;
    // These trajectories describe truth, not constraints imposed on the estimator.
    // They start in motion, independently of the delayed-start maneuver family.
    if (s.scenario == "traffic") {
        // Quintic speed ramps: continuous acceleration/jerk through braking,
        // idling and departure. Geometry depends on distance, never GPS course
        // at zero speed. Small suspension/engine motions persist while stopped.
        double distance = 0, speed = 0, accel = 0, jerk = 0, remaining = time;
        struct Segment {
            double duration, before, after;
        };
        const std::array segments{Segment{20, 8, 8}, Segment{8, 8, 0},
                                  Segment{s.stop_duration_s, 0, 0}, Segment{8, 0, 10},
                                  Segment{86400, 10, 10}};
        for (const auto &segment : segments) {
            const double t = std::min(remaining, segment.duration), u = t / segment.duration;
            const double u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u, u6 = u5 * u;
            const double change = segment.after - segment.before;
            distance += segment.before * t + change * segment.duration * (2.5 * u4 - 3 * u5 + u6);
            speed = segment.before + change * (10 * u3 - 15 * u4 + 6 * u5);
            accel = change * (30 * u2 - 60 * u3 + 30 * u4) / segment.duration;
            jerk = change * (60 * u - 180 * u2 + 120 * u3) / (segment.duration * segment.duration);
            remaining -= t;
            if (remaining <= 0)
                break;
        }
        const double d = distance, v = speed, a = accel;
        const double east = 12 * std::sin(d / 70) + 4 * std::sin(d / 27);
        const double de = 12. / 70 * std::cos(d / 70) + 4. / 27 * std::cos(d / 27);
        const double dde = -12. / (70 * 70) * std::sin(d / 70) - 4. / (27 * 27) * std::sin(d / 27);
        const double down = .3 * std::sin(d / 50), dz = .006 * std::cos(d / 50),
                     ddz = -.00012 * std::sin(d / 50);
        const double phase = std::fmod(.61803398875 * s.seed, 2 * pi);
        const double bounce_w = 2 * pi * 1.3, engine_w = 2 * pi * 17;
        const double bounce = .00005 + .0004 * v, bounce_d = .0004 * a, bounce_dd = .0004 * jerk;
        const double sb = std::sin(bounce_w * time + phase), cb = std::cos(bounce_w * time + phase);
        const double se = std::sin(engine_w * time + phase), ce = std::cos(engine_w * time + phase);
        out.state.position() = Vec3(d, east, down + bounce * sb + 2e-6 * se);
        out.state.velocity() =
            Vec3(v, de * v, dz * v + bounce_d * sb + bounce * bounce_w * cb + 2e-6 * engine_w * ce);
        out.acceleration =
            Vec3(a, dde * v * v + de * a,
                 ddz * v * v + dz * a + bounce_dd * sb + 2 * bounce_d * bounce_w * cb -
                     bounce * bounce_w * bounce_w * sb - 2e-6 * engine_w * engine_w * se);
        heading = std::atan(de) + .003 * degrees * v * std::sin(.3 * time);
        hd = dde * v / (1 + de * de) +
             .003 * degrees * (a * std::sin(.3 * time) + .3 * v * std::cos(.3 * time));
        const double tilt = degrees * (.02 + .006 * v), tilt_d = degrees * .006 * a;
        roll += 2 * degrees * std::sin(d / 40) + tilt * std::sin(2 * pi * 1.1 * time + phase) +
                .003 * degrees * se;
        rd = 2 * degrees * std::cos(d / 40) * v / 40 +
             tilt_d * std::sin(2 * pi * 1.1 * time + phase) +
             tilt * 2 * pi * 1.1 * std::cos(2 * pi * 1.1 * time + phase) +
             .003 * degrees * engine_w * ce;
        pitch += -std::atan(dz) - .15 * degrees * a + tilt * .5 * sb;
        pd = -ddz * v / (1 + dz * dz) - .15 * degrees * jerk +
             .5 * (tilt_d * sb + tilt * bounce_w * cb);
    } else if (s.scenario == "curved_ship" || s.scenario == "winding_road") {
        const double u = time;
        if (s.scenario == "curved_ship") {
            out.state.position() =
                Vec3{6 * u + 3 * std::sin(.05 * u), 40 * (1 - std::cos(.035 * u)),
                     .7 * std::sin(.8 * u) + .2 * std::sin(1.3 * u)};
            out.state.velocity() = Vec3{6 + .15 * std::cos(.05 * u), 1.4 * std::sin(.035 * u),
                                        .56 * std::cos(.8 * u) + .26 * std::cos(1.3 * u)};
            out.acceleration = {-.0075 * std::sin(.05 * u), .049 * std::cos(.035 * u),
                                -.448 * std::sin(.8 * u) - .338 * std::sin(1.3 * u)};
            roll += degrees * (8 * std::sin(.75 * u) + 2 * std::sin(1.1 * u + .2));
            rd = degrees * (6 * std::cos(.75 * u) + 2.2 * std::cos(1.1 * u + .2));
            pitch += 4 * degrees * std::sin(.55 * u + .4);
            pd = 2.2 * degrees * std::cos(.55 * u + .4);
        } else {
            out.state.position() =
                Vec3{12 * u + 20 * std::sin(.12 * u),
                     40 * (1 - std::cos(.15 * u)) + 10 * std::sin(.43 * u), .5 * std::sin(.12 * u)};
            out.state.velocity() =
                Vec3{12 + 2.4 * std::cos(.12 * u), 6 * std::sin(.15 * u) + 4.3 * std::cos(.43 * u),
                     .06 * std::cos(.12 * u)};
            out.acceleration = {-.288 * std::sin(.12 * u),
                                .9 * std::cos(.15 * u) - 1.849 * std::sin(.43 * u),
                                -.0072 * std::sin(.12 * u)};
            roll += 2 * degrees * std::sin(.65 * u);
            rd = 1.3 * degrees * std::cos(.65 * u);
            pitch += degrees * std::sin(.4 * u);
            pd = .4 * degrees * std::cos(.4 * u);
        }
        const auto v = out.state.velocity();
        heading = std::atan2(v.y(), v.x());
        hd = (v.x() * out.acceleration.y() - v.y() * out.acceleration.x()) /
             (v.x() * v.x() + v.y() * v.y());
        if (s.scenario == "curved_ship") {
            heading += 3 * degrees * std::sin(.06 * u);
            hd += .18 * degrees * std::cos(.06 * u);
        }
    } else if (s.scenario == "straight") {
        out.state.position() = Vec3(8 * time, 0, 0);
        out.state.velocity() = Vec3(8, 0, 0);
    } else if (s.scenario != "static" && moving) {
        const bool aircraft = s.scenario == "aircraft";
        const bool ship = s.scenario == "ship";
        const Vec3 amplitude =
            aircraft ? Vec3(40, 25, 8) : (ship ? Vec3(4, 3, .7) : Vec3(10, 6, 2));
        const Vec3 frequency = ship ? Vec3(.18, .25, .8) : Vec3(.4, .6, .35);
        for (int i = 0; i < 3; ++i) {
            const double w = frequency[i], a = amplitude[i];
            out.state.position()[i] = a * (w * t - std::sin(w * t));
            out.state.velocity()[i] = a * w * (1 - std::cos(w * t));
            out.acceleration[i] = a * w * w * std::sin(w * t);
        }
        roll += (aircraft ? .45 : .18) * (1 - std::cos(.7 * t));
        rd = (aircraft ? .45 : .18) * .7 * std::sin(.7 * t);
        pitch += .13 * (1 - std::cos(.5 * t));
        pd = .13 * .5 * std::sin(.5 * t);
        heading += .4 * (1 - std::cos(.3 * t));
        hd = .12 * std::sin(.3 * t);
        if (s.scenario == "rotation") {
            out.state.position().setZero();
            out.state.velocity().setZero();
            out.acceleration.setZero();
        }
    }
    out.state.attitude() =
        Quat(Eigen::AngleAxisd(heading, Vec3::UnitZ()) * Eigen::AngleAxisd(pitch, Vec3::UnitY()) *
             Eigen::AngleAxisd(roll, Vec3::UnitX()));
    out.body_rate = {rd - hd * std::sin(pitch),
                     pd * std::cos(roll) + hd * std::sin(roll) * std::cos(pitch),
                     -pd * std::sin(roll) + hd * std::cos(roll) * std::cos(pitch)};
    return out;
}
Simulation simulation_configuration(const nlohmann::json &j) {
    Simulation s;
    const auto known = simulation_json(s);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.contains(it.key()))
            throw std::invalid_argument("Unknown simulation setting: " + it.key());
    s.scenario = j.value("scenario", s.scenario);
    s.duration_s = j.value("duration_s", s.duration_s);
    s.stop_duration_s = j.value("stop_duration_s", s.stop_duration_s);
    s.start_delay_s = j.value("start_delay_s", s.start_delay_s);
    s.heading = j.value("heading_deg", s.heading / degrees) * degrees;
    s.seed = j.value("seed", s.seed);
    s.noise = j.value("noise", s.noise);
    s.startup_zero = j.value("startup_zero", s.startup_zero);
    s.clock_ppm = j.value("clock_ppm", s.clock_ppm);
    s.sensor_bandwidth_hz = j.value("sensor_bandwidth_hz", s.sensor_bandwidth_hz);
    s.latitude = j.value("latitude_deg", s.latitude / degrees) * degrees;
    s.longitude = j.value("longitude_deg", s.longitude / degrees) * degrees;
    s.height = j.value("height_m", s.height);
    s.roll = j.value("roll_deg", s.roll / degrees) * degrees;
    s.pitch = j.value("pitch_deg", s.pitch / degrees) * degrees;
    s.cross_axis = j.value("cross_axis", s.cross_axis);
    s.gyro_g_sensitivity = j.value("gyro_g_sensitivity", s.gyro_g_sensitivity);
    s.gyro_bias_tau_s = j.value("gyro_bias_tau_s", s.gyro_bias_tau_s);
    s.accel_bias_tau_s = j.value("accel_bias_tau_s", s.accel_bias_tau_s);
    s.imu_latency_s = j.value("imu_latency_s", s.imu_latency_s);
    s.imu_jitter_s = j.value("imu_jitter_s", s.imu_jitter_s);
    s.gnss_latency_s = j.value("gnss_latency_s", s.gnss_latency_s);
    s.gnss_jitter_s = j.value("gnss_jitter_s", s.gnss_jitter_s);
    s.gnss_white_position_sigma = j.value("gnss_white_position_sigma", s.gnss_white_position_sigma);
    s.gnss_stationary_error_start =
        j.value("gnss_stationary_error_start", s.gnss_stationary_error_start);
    s.gnss_position_sigma = j.value("gnss_position_sigma", s.gnss_position_sigma);
    s.gnss_velocity_sigma = j.value("gnss_velocity_sigma", s.gnss_velocity_sigma);
    s.gnss_correlation_s = j.value("gnss_correlation_s", s.gnss_correlation_s);
    s.dropout_start_s = j.value("dropout_start_s", s.dropout_start_s);
    s.dropout_end_s = j.value("dropout_end_s", s.dropout_end_s);
    s.corrupt_every = j.value("corrupt_every", s.corrupt_every);
    s.drop_every = j.value("drop_every", s.drop_every);
    s.duplicate_every = j.value("duplicate_every", s.duplicate_every);
    auto vec = [&](const char *key, Vec3 &v) {
        if (j.contains(key)) {
            auto a = j.at(key).get<std::array<double, 3>>();
            v = Eigen::Map<Vec3>(a.data());
        }
    };
    vec("gyro_bias_rad_s", s.gyro_bias);
    vec("accel_bias_m_s2", s.accel_bias);
    vec("gnss_lever_m", s.gnss_lever);
    vec("gyro_scale_ppm", s.gyro_scale_ppm);
    vec("accel_scale_ppm", s.accel_scale_ppm);
    vec("gyro_temperature_rad_s_K", s.gyro_temperature);
    vec("accel_temperature_m_s2_K", s.accel_temperature);
    vec("gyro_noise_rad_sqrt_s", s.sensor_noise.gyro_density);
    vec("accel_noise_m_s_sqrt_s", s.sensor_noise.accel_density);
    vec("gyro_bias_walk", s.sensor_noise.gyro_bias_walk);
    vec("accel_bias_walk", s.sensor_noise.accel_bias_walk);
    for (double x : {s.stop_duration_s,
                     s.duration_s,
                     s.start_delay_s,
                     s.heading,
                     s.roll,
                     s.pitch,
                     s.latitude,
                     s.longitude,
                     s.height,
                     s.clock_ppm,
                     s.sensor_bandwidth_hz,
                     s.gnss_correlation_s,
                     s.gyro_bias_tau_s,
                     s.accel_bias_tau_s,
                     s.cross_axis,
                     s.gyro_g_sensitivity,
                     s.imu_latency_s,
                     s.imu_jitter_s,
                     s.gnss_latency_s,
                     s.gnss_jitter_s,
                     s.gnss_white_position_sigma,
                     s.gnss_position_sigma,
                     s.gnss_velocity_sigma,
                     s.dropout_start_s,
                     s.dropout_end_s})
        if (!std::isfinite(x))
            throw std::invalid_argument("Non-finite simulation setting");
    for (const auto *v : {&s.gyro_bias, &s.accel_bias, &s.gnss_lever, &s.gyro_scale_ppm,
                          &s.accel_scale_ppm, &s.gyro_temperature, &s.accel_temperature,
                          &s.sensor_noise.gyro_density, &s.sensor_noise.accel_density,
                          &s.sensor_noise.gyro_bias_walk, &s.sensor_noise.accel_bias_walk})
        if (!v->allFinite())
            throw std::invalid_argument("Non-finite simulation vector");
    if (s.stop_duration_s <= 0 || s.stop_duration_s > 3600 || s.duration_s <= 0 ||
        s.duration_s > 86400 || s.start_delay_s < 0 || std::abs(s.latitude) > pi / 2 ||
        std::abs(s.longitude) > pi || std::abs(s.clock_ppm) > 1000 || s.sensor_bandwidth_hz <= 0 ||
        s.gnss_correlation_s <= 0 || s.gyro_bias_tau_s <= 0 || s.accel_bias_tau_s <= 0)
        throw std::invalid_argument("Invalid simulation duration/clock/bandwidth");
    for (double x : {s.imu_latency_s, s.imu_jitter_s, s.gnss_latency_s, s.gnss_jitter_s,
                     s.gnss_white_position_sigma, s.gnss_position_sigma, s.gnss_velocity_sigma})
        if (x < 0 || x > 1000)
            throw std::invalid_argument("Invalid simulation error/latency");
    if (s.scenario != "static" && s.scenario != "straight" && s.scenario != "maneuver" &&
        s.scenario != "aircraft" && s.scenario != "ship" && s.scenario != "rotation" &&
        s.scenario != "curved_ship" && s.scenario != "winding_road" && s.scenario != "traffic")
        throw std::invalid_argument("Unknown trajectory scenario");
    return s;
}
nlohmann::json simulation_json(const Simulation &s) {
    auto a = [](const Vec3 &v) { return std::vector<double>(v.data(), v.data() + 3); };
    return {{"scenario", s.scenario},
            {"duration_s", s.duration_s},
            {"stop_duration_s", s.stop_duration_s},
            {"start_delay_s", s.start_delay_s},
            {"heading_deg", s.heading / degrees},
            {"latitude_deg", s.latitude / degrees},
            {"longitude_deg", s.longitude / degrees},
            {"height_m", s.height},
            {"roll_deg", s.roll / degrees},
            {"pitch_deg", s.pitch / degrees},
            {"seed", s.seed},
            {"noise", s.noise},
            {"startup_zero", s.startup_zero},
            {"clock_ppm", s.clock_ppm},
            {"sensor_bandwidth_hz", s.sensor_bandwidth_hz},
            {"imu_latency_s", s.imu_latency_s},
            {"imu_jitter_s", s.imu_jitter_s},
            {"gnss_latency_s", s.gnss_latency_s},
            {"gnss_jitter_s", s.gnss_jitter_s},
            {"gnss_position_sigma", s.gnss_position_sigma},
            {"gnss_white_position_sigma", s.gnss_white_position_sigma},
            {"gnss_stationary_error_start", s.gnss_stationary_error_start},
            {"gnss_velocity_sigma", s.gnss_velocity_sigma},
            {"gnss_correlation_s", s.gnss_correlation_s},
            {"dropout_start_s", s.dropout_start_s},
            {"dropout_end_s", s.dropout_end_s},
            {"drop_every", s.drop_every},
            {"corrupt_every", s.corrupt_every},
            {"duplicate_every", s.duplicate_every},
            {"gyro_bias_rad_s", a(s.gyro_bias)},
            {"accel_bias_m_s2", a(s.accel_bias)},
            {"gnss_lever_m", a(s.gnss_lever)},
            {"gyro_scale_ppm", a(s.gyro_scale_ppm)},
            {"accel_scale_ppm", a(s.accel_scale_ppm)},
            {"gyro_temperature_rad_s_K", a(s.gyro_temperature)},
            {"accel_temperature_m_s2_K", a(s.accel_temperature)},
            {"cross_axis", s.cross_axis},
            {"gyro_g_sensitivity", s.gyro_g_sensitivity},
            {"gyro_bias_tau_s", s.gyro_bias_tau_s},
            {"accel_bias_tau_s", s.accel_bias_tau_s},
            {"gyro_noise_rad_sqrt_s", a(s.sensor_noise.gyro_density)},
            {"accel_noise_m_s_sqrt_s", a(s.sensor_noise.accel_density)},
            {"gyro_bias_walk", a(s.sensor_noise.gyro_bias_walk)},
            {"accel_bias_walk", a(s.sensor_noise.accel_bias_walk)},
            {"assumptions",
             "WGS84 J2 gravity; 18 GPS leap seconds; noise from 100 Hz capture; bias "
             "drift, transfer function, temperature and GNSS correlation provisional"}};
}

void simulate(const Simulation &s, const std::function<void(const Record &)> &emit) {
    std::mt19937_64 rng(s.seed);
    std::normal_distribution<double> normal;
    auto white = [&]() { return Vec3(normal(rng), normal(rng), normal(rng)); };
    Earth earth(s.latitude, s.longitude, s.height);
    const Time utc_base = std::chrono::duration_cast<Time>(
        std::chrono::sys_days{std::chrono::year{2026} / 10 / 25}.time_since_epoch());
    Record meta;
    meta.kind = Kind::metadata;
    meta.source = Source::simulator;
    meta.detail = simulation_json(s).dump();
    meta.stamp.utc = utc_base;
    emit(meta);
    const double dt = 1 / (200 * (1 + s.clock_ppm * 1e-6));
    Vec3 bg = s.gyro_bias, ba = s.accel_bias, position_error = Vec3::Zero();
    if (s.noise && s.gnss_stationary_error_start)
        position_error = s.gnss_position_sigma * white();
    Vec3 gyro_filtered = Vec3::Zero(), accel_filtered = Vec3::Zero();
    const auto initial = trajectory(s, 0);
    Vec3 zero = s.startup_zero
                    ? Vec3(s.gyro_bias + initial.state.attitude().conjugate() * earth.omega)
                    : Vec3::Zero();
    struct Event {
        Record record;
        std::uint64_t order;
    };
    auto later = [](const Event &a, const Event &b) {
        return a.record.stamp.arrival == b.record.stamp.arrival
                   ? a.order > b.order
                   : a.record.stamp.arrival > b.record.stamp.arrival;
    };
    std::priority_queue<Event, std::vector<Event>, decltype(later)> events(later);
    std::uint64_t order = 0;
    Time last_delivery{};
    auto enqueue = [&](Record record) { events.push({std::move(record), order++}); };
    double next_gnss = 0;
    for (std::uint64_t i = 0; i <= std::uint64_t(s.duration_s / dt); ++i) {
        const double t = i * dt;
        auto truth = trajectory(s, t);
        const double temperature_change = 8 * (1 - std::exp(-t / 120));
        if (s.noise) {
            const double pg = std::exp(-dt / s.gyro_bias_tau_s),
                         pa = std::exp(-dt / s.accel_bias_tau_s);
            bg = s.gyro_bias + pg * (bg - s.gyro_bias) +
                 s.sensor_noise.gyro_bias_walk.cwiseProduct(white()) *
                     std::sqrt(s.gyro_bias_tau_s * .5 * (1 - pg * pg));
            ba = s.accel_bias + pa * (ba - s.accel_bias) +
                 s.sensor_noise.accel_bias_walk.cwiseProduct(white()) *
                     std::sqrt(s.accel_bias_tau_s * .5 * (1 - pa * pa));
        }
        Vec3 w = truth.body_rate + truth.state.attitude().conjugate() * earth.omega;
        Vec3 a = truth.state.attitude().conjugate() *
                 (truth.acceleration - earth.gravity(Vec3(truth.state.position())) +
                  2 * earth.omega.cross(truth.state.velocity()));
        if (!i) {
            gyro_filtered = w;
            accel_filtered = a;
        }
        const double alpha = 1 - std::exp(-2 * pi * s.sensor_bandwidth_hz * dt);
        gyro_filtered += alpha * (w - gyro_filtered);
        accel_filtered += alpha * (a - accel_filtered);
        w = gyro_filtered;
        a = accel_filtered;
        const Vec3 effective_bg =
            bg - zero + (s.noise ? Vec3(s.gyro_temperature * temperature_change) : Vec3::Zero());
        const Vec3 effective_ba =
            ba + (s.noise ? Vec3(s.accel_temperature * temperature_change) : Vec3::Zero());
        if (s.noise) {
            Mat3 m = Mat3::Identity();
            m(0, 1) = s.cross_axis;
            m(1, 2) = -s.cross_axis;
            m(2, 0) = s.cross_axis;
            w = m * ((Vec3::Ones() + s.gyro_scale_ppm * 1e-6).cwiseProduct(w)) +
                s.gyro_g_sensitivity * a;
            a = m * ((Vec3::Ones() + s.accel_scale_ppm * 1e-6).cwiseProduct(a));
            // Density is the Brownian increment coefficient (Allan convention).
            w += s.sensor_noise.gyro_density.cwiseProduct(white()) / std::sqrt(dt);
            a += s.sensor_noise.accel_density.cwiseProduct(white()) / std::sqrt(dt);
        }
        w += effective_bg;
        a += effective_ba;
        w = w.cwiseMax(Vec3::Constant(-400 * degrees)).cwiseMin(Vec3::Constant(400 * degrees));
        a = a.cwiseMax(Vec3::Constant(-2 * standard_gravity))
                .cwiseMin(Vec3::Constant(2 * standard_gravity));
        truth.state.gyro_bias() = effective_bg;
        truth.state.accel_bias() = effective_ba;
        Record tr;
        tr.kind = Kind::truth;
        tr.source = Source::simulator;
        tr.stamp.measurement = time_from_seconds(t);
        tr.stamp.arrival = tr.stamp.measurement;
        tr.stamp.utc = utc_base + tr.stamp.arrival;
        tr.stamp.sequence = i;
        tr.values.assign(truth.state.data.begin(), truth.state.data.end());
        enqueue(std::move(tr));
        if (!s.drop_every || (i + 1) % s.drop_every) {
            Record r;
            r.source = Source::imu;
            r.stamp.sequence = i;
            r.native_counter = i % 256;
            r.stamp.measurement = time_from_seconds(t);
            r.stamp.arrival =
                std::max(last_delivery + time_from_seconds(330. / 460800),
                         time_from_seconds(t + s.imu_latency_s +
                                           std::max(0., s.imu_jitter_s * normal(rng))));
            last_delivery = r.stamp.arrival;
            r.stamp.utc = utc_base + r.stamp.arrival;
            r.payload = encode_imu(w, a, static_cast<std::uint8_t>(i));
            if (s.corrupt_every && (i + 1) % s.corrupt_every == 0)
                r.payload[8] ^= 0x80;
            enqueue(r);
            if (s.duplicate_every && (i + 1) % s.duplicate_every == 0)
                enqueue(r);
        }
        while (next_gnss <= t && next_gnss <= s.duration_s) {
            const auto gt = trajectory(s, next_gnss);
            GnssFix fix;
            Vec3 p = gt.state.position() + gt.state.attitude() * s.gnss_lever;
            Vec3 v = gt.state.velocity() + gt.state.attitude() * (gt.body_rate.cross(s.gnss_lever));
            if (s.noise) {
                const double phi = std::exp(-.1 / s.gnss_correlation_s);
                position_error = phi * position_error +
                                 std::sqrt(1 - phi * phi) * s.gnss_position_sigma * white();
                p += position_error;
                if (s.gnss_white_position_sigma > 0)
                    p += s.gnss_white_position_sigma * white();
                v += s.gnss_velocity_sigma * white();
            }
            Vec3 llh = ecef_to_geodetic(earth.origin + earth.to_ecef * p);
            fix.latitude = llh.x();
            fix.longitude = llh.y();
            fix.height = llh.z();
            fix.velocity = ned_to_ecef(fix.latitude, fix.longitude).transpose() * earth.to_ecef * v;
            fix.position_sigma = Vec3::Constant(std::max(.01, s.gnss_position_sigma));
            fix.velocity_sigma = Vec3::Constant(std::max(.001, s.gnss_velocity_sigma));
            fix.satellites = 12;
            fix.valid = !(next_gnss >= s.dropout_start_s && next_gnss < s.dropout_end_s);
            Record r;
            r.source = Source::gnss;
            r.stamp.measurement = time_from_seconds(next_gnss);
            r.stamp.arrival = time_from_seconds(
                next_gnss + std::max(0., s.gnss_latency_s + s.gnss_jitter_s * normal(rng)));
            r.stamp.utc = utc_base + r.stamp.arrival;
            r.payload = encode_gnss(fix, utc_base + r.stamp.measurement);
            enqueue(std::move(r));
            next_gnss += .1;
        }
        while (!events.empty() && events.top().record.stamp.arrival <= time_from_seconds(t)) {
            emit(events.top().record);
            events.pop();
        }
    }
    while (!events.empty()) {
        emit(events.top().record);
        events.pop();
    }
}
} // namespace ins

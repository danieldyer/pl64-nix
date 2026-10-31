#include "ins/model.hpp"
#include <Eigen/LU>
#include <algorithm>
#include <stdexcept>

namespace ins {
Vec3 geodetic_to_ecef(double lat, double lon, double h) {
    constexpr double a = 6378137., e2 = 6.6943799901413165e-3;
    const double n = a / std::sqrt(1 - e2 * std::sin(lat) * std::sin(lat));
    return {(n + h) * std::cos(lat) * std::cos(lon), (n + h) * std::cos(lat) * std::sin(lon),
            (n * (1 - e2) + h) * std::sin(lat)};
}
Vec3 ecef_to_geodetic(const Vec3 &p) {
    constexpr double a = 6378137., e2 = 6.6943799901413165e-3;
    const double rho = std::hypot(p.x(), p.y());
    double lat = std::atan2(p.z(), rho * (1 - e2)), h = 0;
    for (int i = 0; i < 8; ++i) {
        const double n = a / std::sqrt(1 - e2 * std::sin(lat) * std::sin(lat));
        h = std::abs(std::cos(lat)) > 1e-8 ? rho / std::cos(lat) - n
                                           : std::abs(p.z()) - n * (1 - e2);
        lat = std::atan2(p.z(), rho * (1 - e2 * n / (n + h)));
    }
    return {lat, std::atan2(p.y(), p.x()), h};
}
Mat3 ned_to_ecef(double lat, double lon) {
    const double s = std::sin(lat), c = std::cos(lat), sl = std::sin(lon), cl = std::cos(lon);
    Mat3 m;
    m << -s * cl, -sl, -c * cl, -s * sl, cl, -c * sl, c, 0, -s;
    return m;
}
Earth::Earth(double lat, double lon, double h)
    : origin(geodetic_to_ecef(lat, lon, h)), to_ecef(ned_to_ecef(lat, lon)),
      omega(to_ecef.transpose() * Vec3(0, 0, 7.292115e-5)) {}
Vec3 Earth::position(const GnssFix &f) const {
    return to_ecef.transpose() * (geodetic_to_ecef(f.latitude, f.longitude, f.height) - origin);
}
Vec3 Earth::velocity(const GnssFix &f) const {
    return to_ecef.transpose() * ned_to_ecef(f.latitude, f.longitude) * f.velocity;
}
Vec6 gnss_prediction(const State &s, const Vec3 &receiver_error, const Vec3 &omega,
                     const Earth &earth, const Config &config) {
    const Vec3 angular = omega - s.gyro_bias() - s.attitude().conjugate() * earth.omega;
    Vec6 z;
    z << s.position() + s.attitude() * config.gnss_lever + receiver_error,
        s.velocity() + s.attitude() * angular.cross(config.gnss_lever);
    return z;
}
Mat6 gnss_covariance(const State &s, const Vec3 &force, const GnssFix &f, const Earth &earth,
                     const Config &config) {
    const Vec3 ps = f.position_sigma.cwiseMax(Vec3::Constant(config.gnss_position_floor));
    const Vec3 vs = f.velocity_sigma.cwiseMax(Vec3::Constant(config.gnss_velocity_floor));
    const Mat3 rotation = earth.to_ecef.transpose() * ned_to_ecef(f.latitude, f.longitude);
    Mat6 covariance = Mat6::Zero();
    covariance.topLeftCorner<3, 3>() =
        rotation * ps.array().square().matrix().asDiagonal() * rotation.transpose();
    covariance.bottomRightCorner<3, 3>() =
        rotation * vs.array().square().matrix().asDiagonal() * rotation.transpose();
    Vec6 timing;
    timing << earth.velocity(f), s.attitude() * (force - s.accel_bias()) +
                                     earth.gravity(Vec3(s.position())) -
                                     2 * earth.omega.cross(s.velocity());
    covariance += timing * timing.transpose() * f.stamp.uncertainty_s * f.stamp.uncertainty_s;
    return covariance;
}
State propagate(State x, std::span<const ImuSample> samples, const Earth &e) {
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const double dt = seconds(samples[i].stamp.measurement - samples[i - 1].stamp.measurement);
        if (dt <= 0 || dt > .05 || samples[i].gap || samples[i].saturated)
            throw std::runtime_error("Discontinuous or saturated IMU interval");
        integrate(x.data.data(), samples[i - 1], samples[i], e);
        x.time = samples[i].stamp.measurement;
    }
    return x;
}
Vec15 difference(const State &a, const State &b) {
    Vec15 d;
    d << b.position() - a.position(), b.velocity() - a.velocity(),
        log_rotation(Quat(a.attitude().conjugate() * b.attitude())), b.gyro_bias() - a.gyro_bias(),
        b.accel_bias() - a.accel_bias();
    return d;
}
State displaced(State a, const Vec15 &d) {
    a.position() += d.segment<3>(0);
    a.velocity() += d.segment<3>(3);
    a.attitude() = a.attitude() * exp_rotation(d.segment<3>(6));
    a.gyro_bias() += d.segment<3>(9);
    a.accel_bias() += d.segment<3>(12);
    return a;
}
Mat15 transition(const State &x, const ImuSample &a, const ImuSample &b, const Earth &e) {
    const double dt = seconds(b.stamp.measurement - a.stamp.measurement);
    const Mat3 r = x.attitude().toRotationMatrix();
    Mat15 f = Mat15::Zero();
    f.block<3, 3>(0, 3) = Mat3::Identity();
    f.block<3, 3>(3, 3) = -2 * skew(e.omega);
    f.block<3, 3>(3, 6) = -r * skew((a.force + b.force) * .5 - x.accel_bias());
    f.block<3, 3>(3, 12) = -r;
    f.block<3, 3>(6, 6) = -skew((a.omega + b.omega) * .5 - x.gyro_bias());
    f.block<3, 3>(6, 9) = -Mat3::Identity();
    for (int j = 0; j < 3; ++j) {
        Vec3 d = Vec3::Zero();
        d[j] = 1;
        f.block<3, 1>(3, j) =
            (e.gravity(Vec3(x.position() + d)) - e.gravity(Vec3(x.position() - d))) * .5;
    }
    return Mat15::Identity() + f * dt + (f * f) * (dt * dt * .5);
}
Mat15 process_covariance(const State &initial, std::span<const ImuSample> samples,
                         const Earth &earth, const Noise &n) {
    Mat15 p = Mat15::Zero();
    State x = initial;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const double dt = seconds(samples[i].stamp.measurement - samples[i - 1].stamp.measurement);
        const Mat15 phi = transition(x, samples[i - 1], samples[i], earth);
        Mat15 q = Mat15::Zero();
        const Mat3 r = x.attitude().toRotationMatrix();
        q.block<3, 3>(3, 3) =
            r * n.accel_density.array().square().matrix().asDiagonal() * r.transpose();
        q.block<3, 3>(6, 6) = n.gyro_density.array().square().matrix().asDiagonal();
        q.block<3, 3>(9, 9) = n.gyro_bias_walk.array().square().matrix().asDiagonal();
        q.block<3, 3>(12, 12) = n.accel_bias_walk.array().square().matrix().asDiagonal();
        // Trapezoidal integral of Phi Q Phi^T. Includes p/v and bias coupling.
        p = (phi * p * phi.transpose() + .5 * dt * (q + phi * q * phi.transpose())).eval();
        integrate(x.data.data(), samples[i - 1], samples[i], earth);
    }
    return (p + p.transpose()) * .5;
}
ImuSample interpolate(const ImuSample &a, const ImuSample &b, Time t) {
    if (t < a.stamp.measurement || t > b.stamp.measurement ||
        b.stamp.measurement <= a.stamp.measurement)
        throw std::invalid_argument("IMU interpolation outside interval");
    const double k =
        seconds(t - a.stamp.measurement) / seconds(b.stamp.measurement - a.stamp.measurement);
    ImuSample c = a;
    c.stamp.measurement = t;
    c.omega = (1 - k) * a.omega + k * b.omega;
    c.force = (1 - k) * a.force + k * b.force;
    c.gap = a.gap || b.gap;
    c.saturated = a.saturated || b.saturated;
    return c;
}
std::vector<ImuSample> interval(std::span<const ImuSample> s, Time start, Time end) {
    if (s.size() < 2 || start < s.front().stamp.measurement || end > s.back().stamp.measurement ||
        end <= start)
        throw std::invalid_argument("IMU history does not cover interval");
    auto at = [&](Time t) {
        auto it = std::lower_bound(s.begin(), s.end(), t,
                                   [](const auto &a, Time t) { return a.stamp.measurement < t; });
        if (it->stamp.measurement == t)
            return *it;
        return interpolate(*(it - 1), *it, t);
    };
    std::vector<ImuSample> out{at(start)};
    auto first = std::upper_bound(s.begin(), s.end(), start,
                                  [](Time t, const auto &a) { return t < a.stamp.measurement; });
    for (; first != s.end() && first->stamp.measurement < end; ++first)
        out.push_back(*first);
    out.push_back(at(end));
    return out;
}
} // namespace ins

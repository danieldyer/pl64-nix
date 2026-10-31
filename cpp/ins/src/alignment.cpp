#include "ins/alignment.hpp"
#include <Eigen/Cholesky>
#include <Eigen/Eigenvalues>
#include <Eigen/SVD>

namespace ins {
namespace {
struct Integrals {
    std::vector<Vec3> force;
    Quat rotation = Quat::Identity();
};
Integrals integrate_body(std::span<const ImuSample> imu, std::span<const GnssFix> fixes,
                         const Vec3 &lever, const Vec3 &bg, const Vec3 &ba) {
    Integrals out;
    Quat rotation = Quat::Identity();
    for (std::size_t k = 1; k < fixes.size(); ++k) {
        auto block = interval(imu, fixes[k - 1].stamp.measurement, fixes[k].stamp.measurement);
        Vec3 sum = -(rotation * (block.front().omega - bg).cross(lever));
        for (std::size_t i = 1; i < block.size(); ++i) {
            const double dt = seconds(block[i].stamp.measurement - block[i - 1].stamp.measurement);
            const Vec3 a = block[i - 1].omega - bg, b = block[i].omega - bg;
            const Quat middle =
                rotation *
                exp_rotation(((3 * a + b) * (dt / 8) + a.cross(b) * (dt * dt / 96)).eval());
            const Quat end =
                (rotation * exp_rotation(((a + b) * (dt / 2) + a.cross(b) * (dt * dt / 12)).eval()))
                    .normalized();
            sum += (rotation * (block[i - 1].force - ba) +
                    2 * (middle * (block[i - 1].force + block[i].force - 2 * ba)) +
                    end * (block[i].force - ba)) *
                   (dt / 6);
            rotation = end;
        }
        sum += rotation * (block.back().omega - bg).cross(lever);
        out.force.push_back(sum);
    }
    out.rotation = rotation;
    return out;
}
} // namespace
std::optional<NavigationState> align(const Config &config, const Earth &earth,
                                     std::span<const ImuSample> imu,
                                     std::span<const GnssFix> observations) {
    // Non-overlapping ~1 s intervals reduce differentiated GNSS velocity noise.
    // Shared endpoints remain correlated in the covariance below.
    std::vector<GnssFix> fixes;
    for (const auto &f : observations)
        if (f.stamp.measurement >= imu.front().stamp.measurement &&
            f.stamp.measurement <= imu.back().stamp.measurement &&
            (fixes.empty() || seconds(f.stamp.measurement - fixes.back().stamp.measurement) >= 1))
            fixes.push_back(f);
    if (fixes.size() < 4)
        return {};
    const auto count = fixes.size() - 1;
    const Eigen::Index rows = 3 * count;
    const Time start = fixes.front().stamp.measurement, end = fixes.back().stamp.measurement;
    const double duration = seconds(end - start);
    const auto body = integrate_body(imu, fixes, config.gnss_lever, Vec3::Zero(), Vec3::Zero());
    Eigen::VectorXd y(rows);
    Eigen::MatrixXd velocity_map = Eigen::MatrixXd::Zero(rows, 3 * fixes.size());
    Eigen::MatrixXd velocity_noise = Eigen::MatrixXd::Zero(3 * fixes.size(), 3 * fixes.size());
    Mat3 cross = Mat3::Zero();
    auto rotation = [&](Time t) {
        return exp_rotation((earth.omega * seconds(t - start)).eval()).toRotationMatrix();
    };
    for (std::size_t k = 0; k < count; ++k) {
        const auto &a = fixes[k], &b = fixes[k + 1];
        const Vec3 va = earth.velocity(a), vb = earth.velocity(b);
        const double dt = seconds(b.stamp.measurement - a.stamp.measurement);
        const Mat3 ra = rotation(a.stamp.measurement), rb = rotation(b.stamp.measurement);
        const Mat3 rm =
            rotation(a.stamp.measurement + (b.stamp.measurement - a.stamp.measurement) / 2);
        // d(E v)/dt = R0 B f + E (g - Omega x v), E=exp(Omega*t).
        // Endpoint omega x lever terms in the body integral remove antenna rotation.
        const Vec3 pa = earth.position(a), pb = earth.position(b);
        y.segment<3>(3 * k) =
            rb * vb - ra * va -
            (ra * (earth.gravity(pa) - earth.omega.cross(va)) +
             4 * rm * (earth.gravity(((pa + pb) * .5).eval()) - earth.omega.cross((va + vb) * .5)) +
             rb * (earth.gravity(pb) - earth.omega.cross(vb))) *
                (dt / 6);
        velocity_map.block<3, 3>(3 * k, 3 * k) = -ra + (ra + 2 * rm) * skew(earth.omega) * (dt / 6);
        velocity_map.block<3, 3>(3 * k, 3 * (k + 1)) =
            rb + (rb + 2 * rm) * skew(earth.omega) * (dt / 6);
        cross += y.segment<3>(3 * k) * body.force[k].transpose();
    }
    const Eigen::JacobiSVD<Mat3> svd(cross, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Mat3 sign = Mat3::Identity();
    sign(2, 2) = (svd.matrixU() * svd.matrixV().transpose()).determinant();
    const Mat3 r0 = svd.matrixU() * sign * svd.matrixV().transpose();
    const Quat attitude = exp_rotation((-earth.omega * duration).eval()) * Quat(r0) * body.rotation;
    Eigen::MatrixXd h(rows, 3), bias_map(rows, 6);
    for (std::size_t k = 0; k < fixes.size(); ++k) {
        const auto &f = fixes[k];
        const Mat3 frame = earth.to_ecef.transpose() * ned_to_ecef(f.latitude, f.longitude);
        const auto sigma = f.velocity_sigma.cwiseMax(Vec3::Constant(config.gnss_velocity_floor));
        Mat3 v = frame * sigma.array().square().matrix().asDiagonal() * frame.transpose();
        const std::size_t a = k ? k - 1 : k, b = k + 1 < fixes.size() ? k + 1 : k;
        const Vec3 acceleration = (earth.velocity(fixes[b]) - earth.velocity(fixes[a])) /
                                  seconds(fixes[b].stamp.measurement - fixes[a].stamp.measurement);
        v +=
            acceleration * acceleration.transpose() * f.stamp.uncertainty_s * f.stamp.uncertainty_s;
        velocity_noise.block<3, 3>(3 * k, 3 * k) = v;
    }
    for (std::size_t k = 0; k < count; ++k)
        h.block<3, 3>(3 * k, 0) = -r0 * skew(body.force[k]);
    const Mat3 information = h.transpose() * h;
    const Eigen::LLT<Mat3> information_factor(information);
    if (information_factor.info() != Eigen::Success)
        return {};
    const Eigen::MatrixXd gain = information_factor.solve(h.transpose());
    // GNSS noise alone is a lower bound on total angle covariance. Reject
    // uninformative geometry before computing the IMU-bias sensitivities.
    const Eigen::MatrixXd gps_noise = velocity_map * velocity_noise * velocity_map.transpose();
    const Mat3 lower_covariance = gain * gps_noise * gain.transpose();
    const Eigen::SelfAdjointEigenSolver<Mat3> lower(lower_covariance);
    if (lower.info() != Eigen::Success || !lower_covariance.allFinite() ||
        lower.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return {};
    Eigen::Matrix<double, 3, 6> endpoint_bias;
    for (int axis = 0; axis < 6; ++axis) {
        const double step = axis < 3 ? 1e-6 : 1e-4;
        Vec3 bg = Vec3::Zero(), ba = Vec3::Zero();
        (axis < 3 ? bg[axis] : ba[axis - 3]) = step;
        const auto plus = integrate_body(imu, fixes, config.gnss_lever, bg, ba);
        const auto minus = integrate_body(imu, fixes, config.gnss_lever, -bg, -ba);
        for (std::size_t k = 0; k < count; ++k)
            bias_map.block<3, 1>(3 * k, axis) = r0 * (plus.force[k] - minus.force[k]) / (2 * step);
        endpoint_bias.col(axis) =
            log_rotation(Quat(minus.rotation.conjugate() * plus.rotation)) / (2 * step);
    }
    Mat6 bias_noise = Mat6::Zero();
    bias_noise.topLeftCorner<3, 3>().diagonal().setConstant(config.gyro_initial_sigma *
                                                            config.gyro_initial_sigma);
    bias_noise.bottomRightCorner<3, 3>().diagonal().setConstant(config.accel_initial_sigma *
                                                                config.accel_initial_sigma);
    Eigen::MatrixXd residual_noise = gps_noise + bias_map * bias_noise * bias_map.transpose();
    const double gyro_density = config.noise.gyro_density.maxCoeff();
    const double accel_density = config.noise.accel_density.maxCoeff();
    const double gyro_variance = gyro_density * gyro_density * duration;
    for (std::size_t k = 0; k < count; ++k) {
        const auto block =
            interval(imu, fixes[k].stamp.measurement, fixes[k + 1].stamp.measurement);
        double force_integral = 0;
        for (std::size_t i = 1; i < block.size(); ++i)
            force_integral += .5 * (block[i].force.norm() + block[i - 1].force.norm()) *
                              seconds(block[i].stamp.measurement - block[i - 1].stamp.measurement);
        const double dt = seconds(fixes[k + 1].stamp.measurement - fixes[k].stamp.measurement);
        // Cauchy-Schwarz bound for correlated integrated gyro noise across the
        // intervals; the factor two also bounds its correlation with final angle.
        // The much larger constant IMU-bias uncertainty above is propagated jointly.
        const double gyro = 2 * count * gyro_variance * force_integral * force_integral;
        // Gravity-gradient/centrifugal lever terms omitted by coarse integration
        // are bounded here; the UKF uses the full antenna observation model.
        const double lever = 4e-6 * config.gnss_lever.norm() * dt;
        residual_noise.block<3, 3>(3 * k, 3 * k).diagonal().array() +=
            gyro + accel_density * accel_density * dt + lever * lever;
    }
    const Mat3 angle_covariance = gain * residual_noise * gain.transpose();
    const Eigen::SelfAdjointEigenSolver<Mat3> uncertainty(angle_covariance);
    if (uncertainty.info() != Eigen::Success || !angle_covariance.allFinite() ||
        uncertainty.eigenvalues().maxCoeff() > std::pow(10 * degrees, 2))
        return {};
    Eigen::VectorXd residual(rows);
    for (std::size_t k = 0; k < count; ++k)
        residual.segment<3>(3 * k) = y.segment<3>(3 * k) - r0 * body.force[k];
    const Eigen::LLT<Eigen::MatrixXd> noise_factor(residual_noise);
    if (noise_factor.info() != Eigen::Success)
        throw std::runtime_error("Invalid coarse alignment covariance");
    const double dof = rows - 3, tail = std::log(10000.);
    if (residual.dot(noise_factor.solve(residual)) > dof + 2 * std::sqrt(dof * tail) + 2 * tail)
        return {};
    NavigationState out;
    out.state.time = end;
    out.state.attitude() = attitude.normalized();
    const auto last_sample = interval(imu, start, end).back();
    const Vec3 angular = last_sample.omega - attitude.conjugate() * earth.omega;
    out.state.position() = earth.position(fixes.back()) - attitude * config.gnss_lever;
    out.state.velocity() =
        earth.velocity(fixes.back()) - attitude * angular.cross(config.gnss_lever);
    out.covariance.setZero();
    State measurement_state = out.state;
    const Mat6 measurement =
        gnss_covariance(measurement_state, last_sample.force, fixes.back(), earth, config);
    out.covariance.topLeftCorner<3, 3>() = measurement.topLeftCorner<3, 3>();
    out.covariance.block<3, 3>(3, 3) = velocity_noise.bottomRightCorner<3, 3>();
    const Vec3 last_acceleration =
        (earth.velocity(fixes.back()) - earth.velocity(fixes[fixes.size() - 2])) /
        seconds(fixes.back().stamp.measurement - fixes[fixes.size() - 2].stamp.measurement);
    const Mat3 position_velocity = earth.velocity(fixes.back()) * last_acceleration.transpose() *
                                   std::pow(fixes.back().stamp.uncertainty_s, 2);
    out.covariance.block<3, 3>(0, 3) = position_velocity;
    out.covariance.block<3, 3>(3, 0) = position_velocity.transpose();
    out.covariance.block<3, 3>(6, 6) = angle_covariance;
    out.covariance.block<6, 6>(9, 9) = bias_noise;
    out.covariance.bottomRightCorner<3, 3>().diagonal().setConstant(
        config.gnss_correlated_position_sigma * config.gnss_correlated_position_sigma);
    out.covariance.block<3, 6>(6, 9) = -gain * bias_map * bias_noise;
    out.covariance.block<6, 3>(9, 6) = out.covariance.block<3, 6>(6, 9).transpose();
    out.covariance.block<3, 3>(6, 3) =
        gain * velocity_map.rightCols<3>() * velocity_noise.bottomRightCorner<3, 3>();
    out.covariance.block<3, 3>(3, 6) = out.covariance.block<3, 3>(6, 3).transpose();
    out.covariance.block<3, 3>(6, 0) =
        gain * velocity_map.rightCols<3>() * position_velocity.transpose();
    out.covariance.block<3, 3>(0, 6) = out.covariance.block<3, 3>(6, 0).transpose();
    Mat18 transport = Mat18::Identity();
    transport.block<3, 3>(6, 6) = body.rotation.toRotationMatrix().transpose();
    transport.block<3, 6>(6, 9) = endpoint_bias;
    out.covariance = (transport * out.covariance * transport.transpose()).eval();
    out.covariance.block<3, 3>(6, 6).diagonal().array() += 2 * gyro_variance;
    const Mat3 r = attitude.toRotationMatrix();
    transport.setIdentity();
    transport.block<3, 3>(0, 6) = r * skew(config.gnss_lever);
    transport.block<3, 3>(0, 15) = -Mat3::Identity();
    transport.block<3, 3>(3, 6) = r * skew(last_sample.omega.cross(config.gnss_lever)) -
                                  skew(earth.omega) * r * skew(config.gnss_lever);
    transport.block<3, 3>(3, 9) = -r * skew(config.gnss_lever);
    out.covariance = (transport * out.covariance * transport.transpose()).eval();
    const Mat3 lever_rate = r * skew(config.gnss_lever);
    out.covariance.block<3, 3>(3, 3) +=
        lever_rate *
        (config.noise.gyro_density.array().square() * config.imu_rate).matrix().asDiagonal() *
        lever_rate.transpose();
    return out;
}
} // namespace ins

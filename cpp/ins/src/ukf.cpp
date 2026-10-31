#include "ins/ukf.hpp"
#include <Eigen/Cholesky>
#include <Eigen/QR>
#include <stdexcept>

namespace ins {
namespace {
constexpr double spread = 18 * .5 * .5, wm0 = (spread - 18) / spread;
constexpr double wc0 = wm0 + 1 - .5 * .5 + 2, weight = 1 / (2 * spread);
struct Point {
    State state;
    Vec3 receiver;
};
Vec18 error(const Point &a, const Point &b) {
    Vec18 e;
    e << difference(a.state, b.state), b.receiver - a.receiver;
    return e;
}
Point add(Point p, const Vec18 &d) {
    p.state = displaced(p.state, d.head<15>());
    p.receiver += d.tail<3>();
    return p;
}
std::array<Point, 37> sigma_points(const Point &mean, const Mat18 &root) {
    std::array<Point, 37> points;
    points[0] = mean;
    for (int i = 0; i < 18; ++i) {
        points[2 * i + 1] = add(mean, std::sqrt(spread) * root.col(i));
        points[2 * i + 2] = add(mean, -std::sqrt(spread) * root.col(i));
    }
    return points;
}
// QR of weighted columns, followed by a signed rank-one Cholesky update for
// the central unscented weight. Never subtract two dense covariance matrices.
template <int N, class Derived>
Eigen::Matrix<double, N, N> qr_root(const Eigen::MatrixBase<Derived> &columns) {
    const auto transpose = columns.transpose().eval();
    Eigen::HouseholderQR<typename decltype(transpose)::PlainObject> qr(transpose);
    Eigen::Matrix<double, N, N> lower =
        qr.matrixQR().topRows(N).template triangularView<Eigen::Upper>().transpose();
    for (int i = 0; i < N; ++i) {
        if (lower(i, i) < 0)
            lower.col(i) *= -1;
        if (!std::isfinite(lower(i, i)) || lower(i, i) <= 0)
            throw std::runtime_error("Unscented square root is rank deficient");
    }
    return lower;
}
template <int N>
void rank_update(Eigen::Matrix<double, N, N> &l, Eigen::Matrix<double, N, 1> x, double sign) {
    for (int k = 0; k < N; ++k) {
        const double r2 = l(k, k) * l(k, k) + sign * x[k] * x[k];
        if (!(r2 > 0) || !std::isfinite(r2))
            throw std::runtime_error("Unscented Cholesky downdate is not positive definite");
        const double r = std::sqrt(r2), c = r / l(k, k), s = x[k] / l(k, k);
        l(k, k) = r;
        for (int i = k + 1; i < N; ++i) {
            l(i, k) = (l(i, k) + sign * s * x[i]) / c;
            x[i] = c * x[i] - s * l(i, k);
        }
    }
}
Mat3 right_jacobian(const Vec3 &v) {
    const double a = v.norm();
    const Mat3 s = skew(v);
    if (a < 1e-5)
        return Mat3::Identity() - .5 * s + s * s / 6;
    return Mat3::Identity() - (1 - std::cos(a)) / (a * a) * s +
           (a - std::sin(a)) / (a * a * a) * s * s;
}
} // namespace
Ukf::Ukf(Config config, Earth earth, State state, Vec3 receiver, const Mat18 &covariance)
    : config_(std::move(config)), earth_(std::move(earth)), state_(state), receiver_(receiver) {
    if (!covariance.allFinite())
        throw std::invalid_argument("Nonfinite UKF covariance");
    Eigen::LLT<Mat18> llt((covariance + covariance.transpose()) * .5);
    if (llt.info() != Eigen::Success)
        throw std::invalid_argument("Invalid UKF covariance");
    root_ = llt.matrixL();
}
void Ukf::propagate(std::span<const ImuSample> samples) {
    if (samples.empty() || samples.front().stamp.measurement != state_.time)
        throw std::invalid_argument("UKF propagation epoch mismatch");
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto &a = samples[i - 1], &b = samples[i];
        const double dt = seconds(b.stamp.measurement - a.stamp.measurement);
        if (!(dt > 0) || dt > .05 || b.gap || b.saturated)
            throw std::invalid_argument("Invalid UKF IMU interval");
        auto points = sigma_points({state_, receiver_}, root_);
        const double phi = std::exp(-dt / config_.gnss_correlation_s);
        Mat18 q = Mat18::Zero();
        q.topLeftCorner<15, 15>() =
            process_covariance(state_, samples.subspan(i - 1, 2), earth_, config_.noise);
        q.bottomRightCorner<3, 3>().diagonal().setConstant(config_.gnss_correlated_position_sigma *
                                                           config_.gnss_correlated_position_sigma *
                                                           (1 - phi * phi));
        Eigen::LLT<Mat18> llt(q);
        if (llt.info() != Eigen::Success)
            throw std::runtime_error("Invalid UKF process covariance");
        for (auto &p : points) {
            integrate(p.state.data.data(), a, b, earth_);
            p.state.time = b.stamp.measurement;
            p.receiver *= phi;
        }
        Point mean = points[0];
        bool converged = false;
        for (int iteration = 0; iteration < 20; ++iteration) {
            Vec18 delta = wm0 * error(mean, points[0]);
            for (std::size_t k = 1; k < points.size(); ++k)
                delta += weight * error(mean, points[k]);
            mean = add(mean, delta);
            if (delta.norm() < 1e-10) {
                converged = true;
                break;
            }
        }
        if (!converged)
            throw std::runtime_error("Unscented manifold mean did not converge");
        Eigen::Matrix<double, 18, 54> columns;
        columns.leftCols(18) = llt.matrixL();
        for (int k = 1; k < 37; ++k)
            columns.col(17 + k) = std::sqrt(weight) * error(mean, points[k]);
        root_ = qr_root<18>(columns);
        rank_update<18>(root_, std::sqrt(std::abs(wc0)) * error(mean, points[0]), wc0 < 0 ? -1 : 1);
        state_ = mean.state;
        receiver_ = mean.receiver;
    }
}
bool Ukf::observe(const GnssFix &fix, const ImuSample &sample) {
    if (fix.stamp.measurement != state_.time)
        throw std::invalid_argument("UKF observation epoch mismatch");
    const Point center{state_, receiver_};
    const auto points = sigma_points(center, root_);
    std::array<Vec6, 37> predicted;
    for (std::size_t i = 0; i < points.size(); ++i)
        predicted[i] =
            gnss_prediction(points[i].state, points[i].receiver, sample.omega, earth_, config_);
    Vec6 mean = wm0 * predicted[0];
    for (int i = 1; i < 37; ++i)
        mean += weight * predicted[i];
    Vec6 observed;
    observed << earth_.position(fix), earth_.velocity(fix);
    Mat6 noise = gnss_covariance(state_, sample.force, fix, earth_, config_);
    Mat6 innovation = noise;
    for (int i = 0; i < 37; ++i) {
        const Vec6 d = predicted[i] - mean;
        innovation += (i ? weight : wc0) * d * d.transpose();
    }
    Eigen::LLT<Mat6> innovation_factor(innovation);
    if (innovation_factor.info() != Eigen::Success)
        throw std::runtime_error("Invalid UKF innovation covariance");
    innovation_squared_ = (observed - mean).dot(innovation_factor.solve(observed - mean));
    // Chi-squared(6), 99.99% acceptance under the stated Gaussian model.
    // Reject a grossly inconsistent fix rather than silently changing its noise.
    if (!std::isfinite(innovation_squared_) || innovation_squared_ > 27.856341236)
        return false;
    Eigen::LLT<Mat6> llt(noise);
    if (llt.info() != Eigen::Success)
        throw std::runtime_error("Invalid UKF observation noise");
    Eigen::Matrix<double, 6, 42> columns;
    columns.leftCols(6) = llt.matrixL();
    Eigen::Matrix<double, 18, 6> cross =
        wc0 * error(center, points[0]) * (predicted[0] - mean).transpose();
    for (int i = 1; i < 37; ++i) {
        columns.col(5 + i) = std::sqrt(weight) * (predicted[i] - mean);
        cross += weight * error(center, points[i]) * (predicted[i] - mean).transpose();
    }
    Mat6 root = qr_root<6>(columns);
    rank_update<6>(root, std::sqrt(std::abs(wc0)) * (predicted[0] - mean), wc0 < 0 ? -1 : 1);
    const Eigen::Matrix<double, 6, 18> tmp =
        root.triangularView<Eigen::Lower>().solve(cross.transpose());
    const Eigen::Matrix<double, 18, 6> gain =
        root.transpose().triangularView<Eigen::Upper>().solve(tmp).transpose();
    const Vec18 increment = gain * (observed - mean);
    const Eigen::Matrix<double, 18, 6> downdates = gain * root;
    for (int i = 0; i < 6; ++i)
        rank_update<18>(root_, downdates.col(i), -1);
    const Point next = add(center, increment);
    Mat18 transport = Mat18::Identity();
    transport.block<3, 3>(6, 6) = right_jacobian(increment.segment<3>(6));
    root_ = qr_root<18>(transport * root_);
    state_ = next.state;
    receiver_ = next.receiver;
    return true;
}
} // namespace ins

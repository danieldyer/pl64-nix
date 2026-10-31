#pragma once
#include "types.hpp"
#include <cmath>

namespace ins {
template <class T> using V3 = Eigen::Matrix<T, 3, 1>;
template <class T> using Q4 = Eigen::Quaternion<T>;
template <class Derived> auto skew(const Eigen::MatrixBase<Derived> &v) {
    using T = typename Derived::Scalar;
    Eigen::Matrix<T, 3, 3> m;
    m << T(0), -v.z(), v.y(), v.z(), T(0), -v.x(), -v.y(), v.x(), T(0);
    return m;
}
template <class Derived> auto exp_rotation(const Eigen::MatrixBase<Derived> &r) {
    using T = typename Derived::Scalar;
    using std::cos;
    using std::sin;
    using std::sqrt;
    const T a2 = r.squaredNorm();
    T w, scale;
    if (a2 < T(1e-12)) {
        w = T(1) - a2 / T(8);
        scale = T(.5) - a2 / T(48);
    } else {
        const T a = sqrt(a2);
        w = cos(a / T(2));
        scale = sin(a / T(2)) / a;
    }
    return Q4<T>(w, scale * r.x(), scale * r.y(), scale * r.z());
}
template <class T> V3<T> log_rotation(Q4<T> q) {
    using std::atan2;
    using std::sqrt;
    if (q.w() < T(0))
        q.coeffs() = -q.coeffs();
    const T s2 = q.vec().squaredNorm();
    if (s2 < T(1e-12))
        return T(2) * q.vec() / q.w();
    const T s = sqrt(s2);
    return (T(2) * atan2(s, q.w()) / s) * q.vec();
}
Vec3 geodetic_to_ecef(double latitude, double longitude, double height);
Vec3 ecef_to_geodetic(const Vec3 &);
Mat3 ned_to_ecef(double latitude, double longitude);
struct Earth {
    Vec3 origin;
    Mat3 to_ecef;
    Vec3 omega;
    Earth(double latitude, double longitude, double height);
    Vec3 position(const GnssFix &) const;
    Vec3 velocity(const GnssFix &) const;
    template <class Derived>
    V3<typename Derived::Scalar> gravity(const Eigen::MatrixBase<Derived> &position) const {
        using T = typename Derived::Scalar;
        // WGS84 GM/J2, including centrifugal acceleration exactly once.
        using std::sqrt;
        const V3<T> r = origin.cast<T>() + to_ecef.cast<T>() * position;
        const T r2 = r.squaredNorm(), radius = sqrt(r2);
        const T z2 = r.z() * r.z() / r2;
        const T j = T(1.5 * 1.082629821313e-3 * 6378137. * 6378137.) / r2;
        const T k = T(-3.986004418e14) / (r2 * radius);
        V3<T> g;
        g << k * r.x() * (T(1) + j * (T(1) - T(5) * z2)),
            k * r.y() * (T(1) + j * (T(1) - T(5) * z2)),
            k * r.z() * (T(1) + j * (T(3) - T(5) * z2));
        g.x() += T(7.292115e-5 * 7.292115e-5) * r.x();
        g.y() += T(7.292115e-5 * 7.292115e-5) * r.y();
        return to_ecef.transpose().cast<T>() * g;
    }
};

template <class T>
void integrate(T *x, const ImuSample &a, const ImuSample &b, const Earth &earth) {
    const double dt = seconds(b.stamp.measurement - a.stamp.measurement);
    Eigen::Map<V3<T>> p(x), v(x + 3);
    Eigen::Map<Q4<T>> q(x + 6);
    const Eigen::Map<const V3<T>> bg(x + 10), ba(x + 13);
    const V3<T> w0 = a.omega.cast<T>() - bg, w1 = b.omega.cast<T>() - bg;
    const V3<T> f0 = a.force.cast<T>() - ba, f1 = b.force.cast<T>() - ba;
    // Linear endpoint rates: Magnus coning term and Simpson integration of
    // rotating specific force retain cross-axis/sculling effects at 200 Hz.
    const V3<T> coning = w0.cross(w1) * T(dt * dt);
    const Q4<T> mid = exp_rotation((-earth.omega * (dt * .5)).eval()).template cast<T>() * q *
                      exp_rotation(((T(3) * w0 + w1) * T(dt / 8) + coning / T(96)).eval());
    const Q4<T> end = (exp_rotation((-earth.omega * dt).eval()).template cast<T>() * q *
                       exp_rotation(((w0 + w1) * T(dt / 2) + coning / T(12)).eval()))
                          .normalized();
    const V3<T> midpoint = p + v * T(dt * .5);
    const V3<T> applied =
        (q * f0 + T(2) * (mid * (f0 + f1)) + end * f1) / T(6) + earth.gravity(midpoint);
    const Mat3 cross = skew(earth.omega);
    const V3<T> next = (Mat3::Identity() + cross * dt).inverse().template cast<T>() *
                       ((Mat3::Identity() - cross * dt).template cast<T>() * v + applied * T(dt));
    p += (v + next) * T(dt * .5);
    v = next;
    q = end;
}
State propagate(State, std::span<const ImuSample>, const Earth &);
Vec15 difference(const State &reference, const State &value);
State displaced(State, const Vec15 &);
Mat15 transition(const State &, const ImuSample &, const ImuSample &, const Earth &);
Mat15 process_covariance(const State &, std::span<const ImuSample>, const Earth &, const Noise &);
using Vec6 = Eigen::Matrix<double, 6, 1>;
using Mat6 = Eigen::Matrix<double, 6, 6>;
// Shared observation/noise definitions for the navigator and validation filters.
Vec6 gnss_prediction(const State &, const Vec3 &receiver_error, const Vec3 &omega, const Earth &,
                     const Config &);
Mat6 gnss_covariance(const State &, const Vec3 &force, const GnssFix &, const Earth &,
                     const Config &);
ImuSample interpolate(const ImuSample &, const ImuSample &, Time);
std::vector<ImuSample> interval(std::span<const ImuSample>, Time, Time);
} // namespace ins

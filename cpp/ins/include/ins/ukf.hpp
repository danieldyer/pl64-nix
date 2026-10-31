#pragma once
#include "model.hpp"

namespace ins {
using Vec18 = Eigen::Matrix<double, 18, 1>;
using Mat18 = Eigen::Matrix<double, 18, 18>;
struct NavigationState {
    State state;
    Vec3 receiver = Vec3::Zero();
    Mat18 covariance = Mat18::Identity();
};
// Square-root unscented filter on position, velocity, SO(3), IMU biases and
// correlated GNSS position error. Attitude must be localized at initialization.
class Ukf {
    Config config_;
    Earth earth_;
    State state_;
    Vec3 receiver_ = Vec3::Zero();
    Mat18 root_ = Mat18::Identity();
    double innovation_squared_ = 0;

  public:
    Ukf(Config, Earth, State, Vec3 receiver, const Mat18 &covariance);
    void propagate(std::span<const ImuSample>);
    bool observe(const GnssFix &, const ImuSample &);
    const State &state() const { return state_; }
    const Vec3 &receiver() const { return receiver_; }
    const Mat18 &root() const { return root_; }
    double innovation_squared() const { return innovation_squared_; }
};
} // namespace ins

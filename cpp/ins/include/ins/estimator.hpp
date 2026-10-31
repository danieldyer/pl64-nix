#pragma once
#include "model.hpp"
#include <memory>

namespace ins {
// Deterministic, single-threaded navigation engine. Measurements carry their
// own epochs; no sensor access or wall-clock dependence lives in this class.
class Estimator {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit Estimator(Config = {});
    ~Estimator();
    Estimator(Estimator &&) noexcept;
    Estimator &operator=(Estimator &&) noexcept;
    void imu(const ImuSample &);
    void gnss(const GnssFix &);
    void reset(std::string reason);
    const Estimate &estimate() const;
    const Earth *earth() const;
};
} // namespace ins

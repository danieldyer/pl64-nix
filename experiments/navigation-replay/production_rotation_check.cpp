#include "ins/model.hpp"

// Test adapter only. Calls the production integrator directly, bypassing UKF,
// alignment and protocol code, to compare its rotations with independent RK4.
extern "C" void production_rotations(int n, const double *t, const double *w, const double *f, double *out) {
    ins::Earth earth(-37.8 * ins::degrees, 145 * ins::degrees, 30);
    earth.omega.setZero(); // Same inertial-frame body integral as the audit.
    ins::State state;
    for (int k = 0; k < n; ++k) {
        if (k) {
            ins::ImuSample a, b;
            a.stamp.measurement = ins::time_from_seconds(t[k - 1]);
            b.stamp.measurement = ins::time_from_seconds(t[k]);
            for (int j = 0; j < 3; ++j) {
                a.omega[j] = w[3 * (k - 1) + j];
                b.omega[j] = w[3 * k + j];
                a.force[j] = f[3 * (k - 1) + j];
                b.force[j] = f[3 * k + j];
            }
            ins::integrate(state.data.data(), a, b, earth);
        }
        const ins::Mat3 rotation = state.attitude().toRotationMatrix();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[9 * k + 3 * i + j] = rotation(i, j);
    }
}

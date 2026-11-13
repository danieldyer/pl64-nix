// Independent RK4 preintegration for the offline coordinate-convention audit.
// Scalar Hamilton quaternions in w,x,y,z order; no Eigen or production INS code.
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
void normalize(double *q) {
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= n;
}
void derivative(const double *q, const double *w, double *d) {
    d[0] = -.5 * (q[1] * w[0] + q[2] * w[1] + q[3] * w[2]);
    d[1] = .5 * (q[0] * w[0] + q[2] * w[2] - q[3] * w[1]);
    d[2] = .5 * (q[0] * w[1] + q[3] * w[0] - q[1] * w[2]);
    d[3] = .5 * (q[0] * w[2] + q[1] * w[1] - q[2] * w[0]);
}
void matrix(const double *q, double *r) {
    const double a = q[0], b = q[1], c = q[2], d = q[3];
    r[0] = 1 - 2 * (c * c + d * d);
    r[1] = 2 * (b * c - a * d);
    r[2] = 2 * (b * d + a * c);
    r[3] = 2 * (b * c + a * d);
    r[4] = 1 - 2 * (b * b + d * d);
    r[5] = 2 * (c * d - a * b);
    r[6] = 2 * (b * d - a * c);
    r[7] = 2 * (c * d + a * b);
    r[8] = 1 - 2 * (b * b + c * c);
}
} // namespace

extern "C" void preintegrate(int n, const double *t, const double *w, const double *f, const double *map,
                             const double *bg, double *out) {
    double q[4] = {1, 0, 0, 0}, fi[3] = {}, bi[9] = {};
    std::fill(out, out + 21, 0.);
    out[0] = out[4] = out[8] = 1;
    for (int k = 1; k < n; ++k) {
        const double dt = t[k] - t[k - 1];
        double a[3] = {}, b[3] = {}, mid[3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                a[i] += map[3 * i + j] * w[3 * (k - 1) + j];
                b[i] += map[3 * i + j] * w[3 * k + j];
            }
            a[i] -= bg[i];
            b[i] -= bg[i];
            mid[i] = .5 * (a[i] + b[i]);
        }
        double qs[4][4], dq[4][4], rs[4][9];
        std::copy(q, q + 4, qs[0]);
        derivative(qs[0], a, dq[0]);
        for (int i = 0; i < 4; ++i)
            qs[1][i] = q[i] + dt * .5 * dq[0][i];
        normalize(qs[1]);
        derivative(qs[1], mid, dq[1]);
        for (int i = 0; i < 4; ++i)
            qs[2][i] = q[i] + dt * .5 * dq[1][i];
        normalize(qs[2]);
        derivative(qs[2], mid, dq[2]);
        for (int i = 0; i < 4; ++i)
            qs[3][i] = q[i] + dt * dq[2][i];
        normalize(qs[3]);
        derivative(qs[3], b, dq[3]);
        for (int s = 0; s < 4; ++s)
            matrix(qs[s], rs[s]);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double f0 = f[3 * (k - 1) + j], f1 = f[3 * k + j], fm = .5 * (f0 + f1);
                fi[i] += dt / 6 *
                         (rs[0][3 * i + j] * f0 + 2 * (rs[1][3 * i + j] + rs[2][3 * i + j]) * fm +
                          rs[3][3 * i + j] * f1);
            }
        for (int i = 0; i < 9; ++i)
            bi[i] += dt / 6 * (rs[0][i] + 2 * rs[1][i] + 2 * rs[2][i] + rs[3][i]);
        for (int i = 0; i < 4; ++i)
            q[i] += dt / 6 * (dq[0][i] + 2 * dq[1][i] + 2 * dq[2][i] + dq[3][i]);
        normalize(q);
        double *o = out + 21 * k;
        matrix(q, o);
        std::copy(fi, fi + 3, o + 9);
        std::copy(bi, bi + 9, o + 12);
    }
}

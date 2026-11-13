"""Independent raw-IMU/GNSS batch identification, separate from the navigator.

For each block, integrate body angular rate with independent scalar RK4 code.
Fit an initial SO(3) attitude, constant gyro/accelerometer biases and velocity.
Compare integrated specific force with GNSS velocity, including Earth rotation
and gravity. Fit nuisance parameters to alternating 5 s chunks; score the chunks
left out. No vehicle heading, zero side velocity or flat-road assumption is used.
"""
import argparse
import ctypes
import itertools
import json
from pathlib import Path
import time

import numpy as np
from scipy.integrate import cumulative_trapezoid
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation

DEG = np.pi / 180
CANDIDATE = np.array([[0., 1, 0], [-1., 0, 0], [0., 0, 1.]])


def ecef(lat, lon, height):
    radius = 6378137 / np.sqrt(1 - 6.6943799901413165e-3 * np.sin(lat)**2)
    return np.column_stack(((radius + height) * np.cos(lat) * np.cos(lon),
                            (radius + height) * np.cos(lat) * np.sin(lon),
                            (radius * (1 - 6.6943799901413165e-3) + height) * np.sin(lat)))


def ned_matrix(lat, lon):
    s, c, sl, cl = np.sin(lat), np.cos(lat), np.sin(lon), np.cos(lon)
    return np.array([[-s*cl, -sl, -c*cl], [-s*sl, cl, -c*sl], [c, 0, -s]])


def gravity_ecef(position):
    radius2 = np.sum(position**2, axis=1)
    z2 = position[:, 2]**2 / radius2
    j = 1.5 * 1.082629821313e-3 * 6378137**2 / radius2
    k = -3.986004418e14 / radius2**1.5
    g = k[:, None] * position * (1 + j[:, None] *
                                 np.column_stack((1-5*z2, 1-5*z2, 3-5*z2)))
    g[:, :2] += 7.292115e-5**2 * position[:, :2]
    return g


class Kernel:
    def __init__(self, library):
        self.lib = ctypes.CDLL(str(Path(library).resolve()))
        ptr = np.ctypeslib.ndpointer(dtype=np.float64, flags="C_CONTIGUOUS")
        self.lib.preintegrate.argtypes = [ctypes.c_int] + [ptr] * 6
        self.lib.preintegrate.restype = None

    def integrate(self, t, w, f, matrix, bias):
        out = np.empty((len(t), 21))
        self.lib.preintegrate(len(t), *[np.ascontiguousarray(v, dtype=np.float64)
                                       for v in (t, w, f, matrix, bias, out)])
        return out


class Block:
    def __init__(self, imu, gps, start, end, name, kernel):
        self.kernel, self.name = kernel, name
        # Use valid fixes, downsampled by native 10 Hz epoch index to about 1 Hz.
        all_g = gps[(gps[:, 0] >= start) & (gps[:, 0] <= end)]
        selected = np.arange(len(all_g)) % 10 == 0
        selected &= (all_g[:, 15].astype(int) & 1) != 0
        g = all_g[selected]
        if len(g) < 20:
            raise ValueError("Too few valid GNSS epochs")
        lo, hi = g[0, 0], g[-1, 0]
        ii = imu[(imu[:, 0] >= lo) & (imu[:, 0] <= hi)]
        if np.any(np.diff(ii[:, 0]) > .02):
            raise ValueError("Recorded IMU gap")
        self.epoch = g[:, 0]
        absolute = np.unique(np.r_[ii[:, 0], self.epoch])
        self.t = absolute - lo
        self.w = np.column_stack([np.interp(absolute, imu[:, 0], imu[:, k]) for k in range(3, 6)])
        self.f = np.column_stack([np.interp(absolute, imu[:, 0], imu[:, k]) for k in range(6, 9)])
        self.query = np.searchsorted(absolute, self.epoch)
        self.sigma = g[:, 12:15]
        self.train = ((np.floor((self.epoch - lo + .05) / 5).astype(int) % 3) != 1)
        self.omega = np.array([7.292115e-5 * np.cos(g[0, 3]), 0,
                               -7.292115e-5 * np.sin(g[0, 3])])
        base = ned_matrix(g[0, 3], g[0, 4])
        positions = ecef(all_g[:, 3], all_g[:, 4], all_g[:, 5])
        velocity = np.array([base.T @ ned_matrix(row[3], row[4]) @ row[6:9] for row in all_g])
        earth = Rotation.from_rotvec((all_g[:, 0] - lo)[:, None] * self.omega)
        inertial_v = earth.apply(velocity)
        self.velocity = np.column_stack([np.interp(self.epoch, all_g[:,0], velocity[:,k])
                                         for k in range(3)])
        rhs = gravity_ecef(positions) @ base - np.cross(self.omega, velocity)
        gi = cumulative_trapezoid(earth.apply(rhs), all_g[:, 0], axis=0, initial=0)
        # Set the integration origin to the first selected valid GNSS epoch.
        gi -= np.array([np.interp(lo, all_g[:, 0], gi[:, k]) for k in range(3)])
        self.z = np.array([np.interp(self.epoch, all_g[:, 0], inertial_v[:, k]-gi[:, k])
                           for k in range(3)]).T
        self.cache_key, self.cache_value = None, None

    def integrals(self, matrix, bg, lag=0):
        key = tuple(np.r_[matrix.ravel(), bg, lag])
        if key != self.cache_key:
            if lag:
                shifted = self.t + lag
                w = np.column_stack([np.interp(shifted, self.t, self.w[:, k]) for k in range(3)])
                f = np.column_stack([np.interp(shifted, self.t, self.f[:, k]) for k in range(3)])
            else:
                w, f = self.w, self.f
            out = self.kernel.integrate(self.t, w, f, matrix, bg * DEG)[self.query]
            self.body_rotation = out[:, :9].reshape(-1, 3, 3)
            self.epoch_w = w[self.query] @ matrix.T - bg * DEG
            self.cache_key, self.cache_value = key, (out[:, 9:12], out[:, 12:21].reshape(-1, 3, 3))
        return self.cache_value

    def initial(self, matrix, accel_sign=1):
        f, _ = self.integrals(matrix, np.zeros(3))
        f = f * accel_sign
        w = np.mean(1/self.sigma[self.train]**2, axis=1)
        a, b = self.z[self.train], f[self.train]
        a = a - np.average(a, axis=0, weights=w)
        b = b - np.average(b, axis=0, weights=w)
        u, s, vt = np.linalg.svd((a * w[:, None]).T @ b)
        sign = np.eye(3)
        sign[2, 2] = np.linalg.det(u @ vt)
        r = u @ sign @ vt
        return np.r_[Rotation.from_matrix(r).as_rotvec(), np.zeros(6)]

    def residual(self, x, matrix, lag=0, gyro_prior=.3, accel_prior=.5,
                 accel_sign=1, scores=False):
        fi, bi = self.integrals(matrix, x[3:6], lag)
        force = accel_sign * fi - np.einsum("nij,j->ni", bi, x[6:9])
        if len(x) > 9:
            # Antenna velocity relative to IMU. Omitted Earth-rate lever term
            # is bounded by 0.00037 m/s for a 5 m lever, far below GNSS noise.
            force += np.einsum("nij,nj->ni", self.body_rotation,
                               np.cross(self.epoch_w, x[9:12]))
        predicted = force @ Rotation.from_rotvec(x[:3]).as_matrix().T
        weights = 1/self.sigma**2
        v0 = np.sum(((self.z-predicted)*weights)[self.train], axis=0) / weights[self.train].sum(axis=0)
        error = predicted + v0 - self.z
        if scores:
            return error
        return np.r_[(error/self.sigma)[self.train].ravel(),
                     x[3:6]/gyro_prior, x[6:9]/accel_prior,
                     x[9:12]/2 if len(x)>9 else []]

    def fit(self, matrix, initial=None, lag=0, gyro_prior=.3, accel_prior=.5, accel_sign=1,
            lever=False):
        x = self.initial(matrix, accel_sign) if initial is None else initial
        if lever and len(x)==9:
            x = np.r_[x, np.zeros(3)]
        fit = least_squares(self.residual, x, args=(matrix, lag, gyro_prior, accel_prior, accel_sign),
                            x_scale=np.r_[np.ones(3), np.ones(3)*.3, np.ones(3)*.5,
                                          np.ones(3) if lever else []],
                            max_nfev=70, ftol=1e-7, xtol=1e-7, gtol=1e-6)
        error = self.residual(fit.x, matrix, lag, gyro_prior, accel_prior, accel_sign, scores=True)
        return {"block": self.name, "parameters": fit.x.tolist(), "cost": float(2*fit.cost),
                "success": bool(fit.success), "evaluations": fit.nfev,
                "heldout_count": int((~self.train).sum()),
                "heldout_sse": float(np.sum(error[~self.train]**2)),
                "heldout_normalized_sse": float(np.sum((error/self.sigma)[~self.train]**2)),
                "train_rmse_mps": float(np.sqrt(np.mean(error[self.train]**2))),
                "heldout_rmse_mps": float(np.sqrt(np.mean(error[~self.train]**2))),
                "heldout_error": error[~self.train].tolist()}


def blocks(root, kernel, duration=40):
    result = []
    for path in sorted((root / "decoded").glob("*.npz"))[:3]:
        data = np.load(path)
        imu, gps = data["imu"], data["gps"]
        start = max(imu[0, 0], gps[0, 0]) + 1
        end = min(imu[-1, 0], gps[-1, 0]) - 1
        for index, lo in enumerate(np.arange(start, end-20, duration)):
            try:
                result.append(Block(imu, gps, lo, min(end, lo+duration),
                                    path.stem + f"-block{index:02}", kernel))
            except ValueError as e:
                print(path.stem, index, str(e), flush=True)
    return result


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("data", type=Path)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--screen", action="store_true")
    p.add_argument("--fits", action="store_true")
    args = p.parse_args()
    root = args.data.resolve()
    kernel = Kernel(args.library)
    bs = blocks(root, kernel)
    out = root / "identification"
    out.mkdir(exist_ok=True)
    if args.screen:
        rows = []
        for perm in itertools.permutations(range(3)):
            for signs in itertools.product((-1, 1), repeat=3):
                matrix = np.zeros((3, 3))
                matrix[np.arange(3), perm] = signs
                label = "".join(map(str, perm)) + "".join("p" if s > 0 else "m" for s in signs)
                total = 0
                for b in bs:
                    x = b.initial(matrix)
                    err = b.residual(x, matrix, scores=True)
                    total += np.sum((err/b.sigma)[~b.train]**2)
                rows.append({"label": label, "matrix": matrix.tolist(), "heldout_score": float(total)})
        rows.sort(key=lambda r:r["heldout_score"])
        (out / "screen.json").write_text(json.dumps(rows, indent=2))
        print(json.dumps(rows[:8], indent=2), flush=True)
    if args.fits:
        for name, matrix in [("identity", np.eye(3)), ("candidate", CANDIDATE),
                              ("opposite_gyro", -np.eye(3)), ("inverse_candidate", CANDIDATE.T)]:
            t = time.time()
            rows = []
            for b in bs:
                fit = b.fit(matrix)
                rows.append(fit)
                print(name, b.name, round(fit["heldout_rmse_mps"], 3),
                      np.round(fit["parameters"][3:6], 3), flush=True)
            (out / (name+".json")).write_text(json.dumps(rows, indent=2))
            print(name, "seconds", time.time()-t, flush=True)


if __name__ == "__main__":
    main()

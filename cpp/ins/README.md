# IMU/GNSS navigation service

C++23 navigation at the IMU reference point. One square-root UKF estimates
position, velocity, attitude, gyro bias, accelerometer bias and correlated GNSS
position error. It includes Earth rotation, gravity, Coriolis acceleration and
the measured GNSS antenna lever arm. It makes no road-vehicle, course-equals-heading
or zero-velocity assumption. Radar processing belongs to downstream consumers.

The state machine is built-in test → coarse alignment → fine alignment →
navigating, with degraded status for missing/invalid measurements. Coarse
alignment matches integrated body specific force to GNSS velocity changes.
It waits for independent directions and a localized attitude before starting
the UKF. Static or constant-velocity data cannot establish heading with unknown
IMU bias. Before fine alignment, the gravity/gyro attitude and latest GNSS
position/velocity are previews, not a time-aligned navigation solution.

Once initialized, the same UKF propagates every IMU sample and corrects at each
GNSS measurement epoch. Bounded checkpoints allow delayed/out-of-order fixes to
be inserted and replayed chronologically without counting observations twice.
The output is propagated to the latest IMU epoch. Fixes older than retained
history and duplicate epochs are ignored. Gross GNSS innovations are rejected
using a six-dimensional chi-squared gate. IMU gaps, saturation and clock
discontinuities invalidate alignment and return to BIT; numerical failures stop
the service.
There is no alternative estimator, numerical repair or fallback filter.

Navigation readiness requires heading, tilt and velocity uncertainty to meet
limits continuously for five seconds. It is a quality indication under the
configured noise model, not an integrity guarantee. Weak motion may never
provide enough information to align. Bias driving noise and absolute sensor
latency still need hardware characterization; timestamps are not PPS synchronized.

## Build and run

```sh
nix build .#pl64-ins
./result/bin/pl64-ins live --config /path/to/settings.json
```

The Nix module is `nixos/ins.nix`. It owns COM1, starts on IMU device activation
(including boot with the USB serial adapter present), records to
`/var/lib/pl64-ins`, and publishes at `/run/pl64-ins/state.sock`.
The known IMU is configured for 200 Hz floating-point output at 460800 baud.
Only the read-only readiness probe is repeated during a baud transition.
Startup gyro-zero is read and logged; no calibration command is issued.
GPS configuration is in `nixos/configure-ublox.sh`: 10 Hz native-USB UBX,
continuous tracking, airborne <4g dynamics, static hold off, unused UART muted.

BIT requires fresh, consecutive valid GNSS fixes at 10 Hz and a continuous
200 Hz IMU history, with finite, unsaturated samples and a usable specific-force
direction. It cannot complete from cached fixes or a previous sensor session.

Sensor lifecycle has one recovery path: end the acquisition session, discard
its parser/clock/filter state, configure sensors, and repeat BIT. The service
publishes and records an invalid BIT state before closing a session. IMU counter
gaps, a 250 ms decoded-sample timeout, or a sustained wrong rate trigger restart.
GPS disconnects, epoch resets, loss of established time, a 3 s NAV-PVT timeout,
or a sustained wrong rate exit with status 75. Epoch advancement is required
once receiver time is valid; unknown time during cold acquisition is allowed.
Cadence is measured between decoded packets, excluding startup waiting time.
The Nix-owned `ExecStopPost` hook requests a gpsd restart only for that status
and only while gpsd is active, so its exclusive
configuration/readback sequence runs again. It does not race an ongoing GPS
configuration or start an absent device. Both services retry unexpected exits
every five seconds without a start-limit lockout; an explicit systemd stop still
stops them. Udev handles removal and replug.

A healthy GNSS stream with no fix keeps startup in BIT; an initialized solution
becomes degraded when fixes age out. Cold acquisition without valid UTC also
waits in BIT, while loss of previously established time triggers reinitialization.
IMU saturation or a system UTC step returns the estimator to BIT
without unnecessary hardware configuration. Consumers must check stage, validity
and message age: an absent/crashed service cannot keep publishing heartbeats.
Socket clients must resubscribe after a service restart.
Nix settings live under `services.pl64-ins.settings`; standard systemd options
control retry policy. Hardware rates/protocols are fixed to the fitted sensors.

Example settings (replace the provisional mounting geometry with measurements):

```json
{
  "navigation": {
    "history_s": 3,
    "gnss_lever_m": [0, 0, 0],
    "sensor_to_body_xyzw": [0, 0, 0, 1],
    "imu_delay_s": 0.001,
    "imu_timing_sigma_s": 0.01,
    "gnss_timing_sigma_s": 0.1
  }
}
```

The lever points from IMU to GNSS antenna in body axes. The mounting quaternion
maps sensor axes to body axes. All other physical parameters and their units
are listed in `configuration_json()` in `src/config.cpp`; unknown navigation
keys are rejected. Use the same settings for capture and replay.

```sh
./result/bin/pl64-ins replay --config settings.json --input capture.mcap
```

## Data interface

Raw bytes, sensor configuration exchanges, decoded measurements and navigation
states use `schema/navigation.fbs` inside uncompressed MCAP. Records retain host
monotonic/UTC timestamps, estimated measurement times, native counters and clock
uncertainty. Files are rotated, finalized on clean shutdown and never overwritten.
Replay verifies MCAP CRCs and FlatBuffers before decoding.

The Unix datagram socket accepts a subscription datagram from a bound local
client and sends serialized FlatBuffer `Record` messages. State values contain
position/velocity in a fixed local NED frame, body-to-frame quaternion xyzw,
IMU biases, ECEF origin and frame rotation. Covariance uses position, velocity,
right-body rotation, gyro bias and accelerometer bias order. Stage/validity flags
and GNSS age must be checked before consuming a navigation solution.

## Tests

`Estimator` accepts timestamped measurements without devices, sleeps or clocks.
The simulator is linked only into tests. It generates binary sensor packets
from independent 3D trajectories, with noise, bias drift, scale/cross-axis errors,
temperature effects, quantization, clock errors, latency and transport faults.

For a local build with Eigen, FlatBuffers, nlohmann-json and MCAP headers available:

```sh
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release -DMCAP_INCLUDE_DIR=/path/to/mcap/include
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests cover sensor frames, rotating-Earth mechanics, analytic Kalman updates,
static/moving alignment, delayed and duplicate GNSS, outages, outliers, sensor
faults and deterministic recording replay. Nix also runs the Linux PTY startup
and sensor lifecycle regressions (including no-fix cold acquisition, silence,
resets, wrong rates and rejected configuration). `ins-capture` is a test utility
for independently decoding recorded raw bytes and checking state/rate/counter/
checksum diagnostics; it is not installed in the service package.

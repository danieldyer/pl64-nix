Offline navigation replay

This builds the actual cpp/ins estimator, alignment, mechanisation and square-root
UKF in a separate directory. No sensor, service or production setting is changed.
Instrumentation records alignment rejection reasons, innovations, uncertainty,
state transitions and held-out GNSS predictions. Legacy configs without
coarse_alignment_history_s retain their original 12-second baseline in audit.cpp;
current production defaults to 60 seconds. analysis_history_s changes coarse-history retention only;
it is different from history_s, the production delayed-update replay buffer.

Python dependencies: numpy, scipy, matplotlib, mcap, flatbuffers.
C++ dependencies: C++23 compiler, Eigen 3.4, nlohmann-json 3.11.

Put the original central MCAP recordings in DATA/originals, then:

  python3 experiments/navigation-replay/decode.py DATA
  python3 experiments/navigation-replay/make_inputs.py DATA
  python3 experiments/navigation-replay/raw_audit.py DATA
  python3 experiments/navigation-replay/build.py /tmp/nav-audit-build \
    --eigen /path/containing/Eigen --json /path/containing/nlohmann
  python3 experiments/navigation-replay/replay.py DATA \
    --binary /tmp/nav-audit-build/nav-audit \
    --variants baseline history60 gyro_frame_hist30 gyro_frame_hist60_calibrated
  python3 experiments/navigation-replay/replay.py DATA \
    --binary /tmp/nav-audit-build/nav-audit --holdout 5 \
    --variants baseline history60 gyro_frame_hist30 gyro_frame_hist60_calibrated
  python3 experiments/navigation-replay/axis_closure.py DATA
  python3 experiments/navigation-replay/report.py DATA

To audit every signed gyro permutation, use replay.py --axis-sweep. These are
diagnostic counterfactuals, including physically improper mappings, not settings
to deploy automatically. The promising proper mapping is [gyro_y,-gyro_x,gyro_z]
relative to the recorded accelerometer coordinates. Controlled physical rotations
remain useful for verifying casing markings and fine calibration. sensor_to_body
rotates both sensors together and cannot correct their relative coordinate frame.
The calibrated variant also swaps the measured gyro X/Y noise densities; isotropic
bias priors and walks are unchanged.
The IMU50 V3.0 manual and V4.0 output protocol do not document a separate 90-degree
gyro frame. Section 2.17 labels both triples X/Y/Z and confirms the existing wire
offsets and units. The mapping therefore comes from measurement identification,
rather than a documented manufacturer frame definition. The initial replay alone
was insufficient to settle the convention; the independent identification below
tests the recorded gyro/accelerometer relationship without the navigation filter.

Build a second copy with build.py --unmodified, then run verify.py DATA
--unmodified /tmp/control/nav-audit --instrumented /tmp/nav-audit-build/nav-audit
--offsets 5 15 30. This compares default outputs exactly and checks how losing
early motion changes initialization; the calibrated configs must exist first.

Replay fidelity and interpretation

Inputs are recorded, decoded measurements with their original measurement and
arrival timestamps, validity, gaps and saturation flags. They enter the production
Estimator API directly. The packet decoder is not re-run by the harness. Record
decoding checks MCAP CRCs; incomplete MCAPs retain their original bytes and the
complete prefix is decoded with the parse error recorded explicitly.

The original estimator was already running when recording started. Replays start
fresh, so alignment-attempt phase and decimation can differ. Coarse geometry near
the admission limit is sensitive to this phase. A recording gap is not evidence
of a sensor gap: inspect the MCAP subscription/sequence metadata. The 2026-11-08
long recording lost about 1.11 s near its end; replay conservatively resets there.

Holdout mode removes every Nth valid fix after the UKF exists, including while
degraded; initial alignment sees the same data as the corresponding full replay.
Predictions are compared at the fix epoch using a short constant-velocity position
projection (dt is logged). Held-out fixes are still correlated with nearby GNSS
measurements and are not independent ground truth. They test predictive
consistency, not absolute position, heading or covariance calibration.

Gravity closure integrates gyro over 6 s and compares 2 s mean accelerometer
directions at the ends, retaining near-g force norms and appreciable rotations.
Vehicle acceleration perturbs this proxy; it is a diagnostic under the recorded
driving conditions, not a zero-acceleration assumption in the navigation filter.
The axis candidate was checked across all three motion recordings. Readiness
thresholds, innovation gates and receiver-reported uncertainty are unchanged.

Build provenance and the exact instrumentation diff are saved beside nav-audit.
Original recordings and generated results belong under ignored artifacts/, not Git.

Independent axis identification

Build independent_preintegration.cpp as a shared library (Linux: -shared -fPIC;
macOS: -dynamiclib). It implements scalar Hamilton-quaternion RK4 and imports no
production INS code. Then run:

  python3 experiments/navigation-replay/identify_axes.py DATA \
    --library /tmp/independent.so --screen --fits
  python3 experiments/navigation-replay/compare_axes.py DATA /tmp/independent.so
  python3 experiments/navigation-replay/profile_axes.py DATA /tmp/independent.so
  python3 experiments/navigation-replay/accel_mount.py DATA
  python3 experiments/navigation-replay/turn_rate_check.py DATA
  python3 experiments/navigation-replay/report_axes.py DATA

The 40 s blocks have independent initial attitude, velocity and constant IMU
biases. Alternating contiguous 5 s GNSS sections are withheld from fitting.
All 48 signed gyro permutations are ranked on the middle run, then checked on
the other runs. Alternative force signs, +/-0.5 s timing offsets, larger bias
priors and antenna lever arms are controls. No car heading, side-slip or road
constraint enters the main physics fit. Block bootstrap intervals preserve
within-block GNSS correlation; they are model comparisons, not integrity bounds.

calibrate_axes.py additionally fits unrestricted SO(3) mappings. One rotation
direction near body down is weakly constrained by the mostly planar motion.
Selection of a signed-axis convention is stronger than a precision arbitrary
three-dimensional extrinsic calibration; do not confuse the two conclusions.

The turn-angle and accelerometer-only mounting checks use the user's ground-car
context explicitly. GPS selects the non-overlapping turns before gyro predictions
are computed. Direction uncertainties receive no sqrt(N) averaging reduction.
The accelerometer-only fit uses GNSS acceleration in the course frame and a
roughly fixed mount, excluding the gyros entirely.

For a body frame defined with +Y mainly down and +Z along the left-looking antenna boresight,
a right-handed physical frame has +X approximately forward. The recordings put
packet-accel +X rearward and +Z right/down. A common diag(-1,1,-1) rotation maps
the packet-accel frame into that physical body-frame definition. It acts on
both corrected gyros and accelerometers and is separate from the gyro-only
mapping [y,-x,z]. Their composition maps packet gyros to [-y,-x,-z] in the
user-defined body frame. Both matrices have determinant +1. This is a mounting
inference from the recorded motion/context, not an assertion about undocumented
manufacturer casing labels.

For integration checks, compile production_rotation_check.cpp with the real
model.cpp, config.cpp, Eigen and nlohmann-json into a second shared library. verify_independent.py DATA
INDEPENDENT_LIBRARY PRODUCTION_LIBRARY compares six analytic +/-90 degree cases,
real variable-rate inputs and common-coordinate invariance. Production rotation
math agrees with independent RK4 to <1e-9 in matrix entries on these recordings.

Interface audit

check_interface.cpp replays .wire serial reads through the current production
Framer, decode_imu, ImuClock and Acquisition. Build it with protocol.cpp, clock.cpp,
acquisition.cpp and config.cpp, then run verify_interface.py DATA BINARY. The
291,007 recorded IMU vectors/counters match exactly after the explicit legacy
gyro normalization in verify_interface.py. Timestamp history
starts at recording attachment, so this check deliberately compares the sensor
values/counters rather than claiming identical pre-attachment clock history.

The original code assumed wire gyro fields already shared the acceleration basis:
protocol.cpp assigns fields 0/1/2 to omega.x/y/z, and acquisition.cpp applies a
single sensor_to_body quaternion to both triples. The data-backed relative
mapping was missing. The original simulator encoder and round-trip tests used the same
assumption, so they verify bytes/units but cannot validate physical axis semantics.
No wire offset, endian, unit conversion or later vector transpose/sign-flip error
was found. The interface needed explicit gyro-to-acceleration-frame normalization
before the common mounting rotation; this does not assert that physical pitch/
roll markings directly name accelerometer axes. The boresight-to-packet mapping
is a separate mounting inference, now accepted for production (see below).

Production integration, accepted 11 November 2026

The real IMU decoder now applies [packet_y,-packet_x,packet_z] to gyros and
leaves accelerometer packet axes unchanged. Acquisition applies the common
mount after normalization. PL64 uses sensor_to_body_xyzw=[0,1,0,0], configured
through services.pl64-ins.imuToAntennaQuaternion: body +Z is antenna boresight,
+Y mainly down, +X completing the right-handed frame. Thus raw packet gyro
vectors become [-y,-x,-z], and acceleration becomes [-x,y,-z]. The gyro noise
densities use the normalized sensor frame; full process-noise covariances are
rotated into the mounted body frame. The production coarse history is now a
validated configuration parameter with a 60-second default. Acceptance gates,
GNSS tuning, bias priors and noise magnitudes were not relaxed.

The original interface check above established exact pre-change packet decoding.
verify_interface.py now compares corrected production values against explicitly
normalized legacy decoded values, so it continues to check all retained serial
samples without requiring new raw data. It is specific to these legacy captures.
The historical playback HTML keeps its original replay and coordinate labels;
its display boresight -packet Z is the new production body's +Z.

The CMake test utility ins-replay-wire exercises the uninstrumented production
Framer, decoder, Acquisition clock/mount and Estimator with retained serial reads:

  build/ins/ins-replay-wire DATA/decoded/SESSION.wire settings.json states.jsonl

Use the effective production navigation settings (no analysis_* fields).
The 8 November raw-byte replays yielded 0, 245.385 and 604.950 seconds navigating
in the short, middle and long runs respectively, with zero IMU checksum errors.
These are nominal 200 Hz sample counts, not independent accuracy measurements.
The long raw stream contains two additional samples versus the retained decoded
stream and rebuilds timing from the recording boundary; tiny differences from
the historical decoded-event replay are expected. New unit fixtures independently
check all signed wire axes and mounting, plus arbitrary-rotation propagation/
covariance invariance. Navigation scenarios cover a non-axis-aligned mount with
a nonzero GNSS lever and delayed fixes.

Offline visual playback of runs 2 and 3

Open artifacts/nav-review-20261108/navigation-playback.html directly in a modern
browser. It contains both recordings, the experimental replay, the original live
estimate, GNSS and a local OpenStreetMap vector extract. It needs no server,
network access, API key or external JavaScript. The generated file is about 13 MB.
The private recordings and generated HTML stay under ignored artifacts/.

Select the run and estimate, scrub the timeline, or play at 1x realtime (other
speeds are available). First navigating jumps past initial alignment. Drag and
scroll the map, Fit route, or Follow the estimate. Toggle the other estimate for
comparison. The attitude pane shows antenna boresight elevation and roll about
the boresight. The default display frame is forward=-packet Z, right=-packet X,
down=+packet Y, matching the earlier motion-inferred mounting. Its opposite is
selectable. This display choice changes neither the replay nor production data.
Do not treat it as a substitute for the deferred physical axis bench test.

Reproduce the page after building nav-audit as above:

  DATA=artifacts/nav-review-20261108
  mkdir -p "$DATA/playback"
  /tmp/nav-audit-build/nav-audit \
    "$DATA/decoded/session-1794105395676964619-682.events" \
    "$DATA/configs/session-1794105395676964619-682-gyro_frame_hist60_calibrated.json" \
    "$DATA/playback/middle.audit.jsonl" 0 "$DATA/playback/middle.states.jsonl"
  /tmp/nav-audit-build/nav-audit \
    "$DATA/decoded/session-1794106036529468891-682.events" \
    "$DATA/configs/session-1794106036529468891-682-gyro_frame_hist60_calibrated.json" \
    "$DATA/playback/long.audit.jsonl" 0 "$DATA/playback/long.states.jsonl"
  python3 experiments/navigation-replay/fetch_playback_map.py "$DATA"
  python3 experiments/navigation-replay/playback.py "$DATA"

fetch_playback_map.py uses curl and caches sixteen small OSM API extracts with
at most two concurrent requests. Only bounding boxes are sent to OSM, not the
trajectories. Attribution and the ODbL link are visible in the viewer. The full
cached source extract and its provenance are in DATA/playback/streets.*; those
sources should accompany any separately redistributed map database.

The optional final nav-audit argument writes a nominal 20 Hz display sidecar.
It does not alter the existing diagnostic output or estimator calculations;
both runs' complete diagnostic streams matched the saved earlier replays
exactly. The sidecar carries the actual ECEF origin and fixed-NED basis for
each state, including after resets. playback.py converts position to WGS84 and
attitude/velocity to current local NED, then projects the map to locally scaled
Mercator. No course alignment or road snapping is applied. Quaternion SLERP is
used only between adjacent display samples; stage/frame changes and missing
data are not interpolated across. The long run's 1.11 s recording gap is shown
explicitly. The fresh replay and already-running recorded estimator have
different pre-recording histories; the selector describes that distinction.

With Python playwright and installed Chrome/Chromium, validate offline playback:

  python3 experiments/navigation-replay/test_playback.py \
    "$DATA/navigation-playback.html" --chrome /path/to/chrome

The browser check disables networking and verifies both runs, actual realtime
progress, scrubbing, keyboard steps, start/end, comparison, mounting reversal,
analytic attitude fixtures, alignment boundaries, the IMU gap, and small/large
layouts. Screenshots and report.json go in DATA/playback/browser-check/.

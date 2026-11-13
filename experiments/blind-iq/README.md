# Blind DC/IQ correction qualification

These seeded numerical checks informed the autonomous corrector now implemented
in the sibling usdr-fpga repository. Operational RTL has its own fixed-point
solver and waveform regressions.

Use a C++23 compiler and the sibling radar source, plus Python with NumPy,
SciPy and Matplotlib. Keep generated waveform files outside Git:

```sh
c++ -std=c++23 -O3 -I../dvbt_passive_radar/src \
  experiments/blind-iq/carriers.cpp ../dvbt_passive_radar/src/modem.cpp \
  -o /tmp/dvbt-carriers
/tmp/dvbt-carriers /tmp/one.cf32 544 9351
/tmp/dvbt-carriers /tmp/two.cf32 544 6437
/tmp/dvbt-carriers /tmp/three.cf32 544 1381
/tmp/dvbt-carriers /tmp/c31.cf32 544 3251 0
python3 experiments/blind-iq/evaluate.py /tmp/{one,two,three}.cf32 \
  --c31 /tmp/c31.cf32 --output /tmp/blind-iq.json
python3 experiments/blind-iq/adaptation.py /tmp/{one,two,three}.cf32 /tmp/adaptation.json
python3 experiments/blind-iq/plot.py /tmp/blind-iq.json /tmp/blind-iq.png
```

`carriers.cpp` writes frequency-domain 6817-carrier CF32 symbols. Python builds
time waveforms and band placements. QPSK and 64-QAM use the same 3/4 FEC rate and
1/16 guard interval. Tests use known injected errors only for scoring, never
for estimating correction coefficients. Numerical division/square root is a
floating-point golden solver; moments/ADC/coefficient/offset/output precision
are covered, with production fixed-point solver checks maintained in usdr-fpga/tests.

The drift experiment is a sequence of windows whose analog matrix changes at
window boundaries, with coefficients from the previous window. It does not
claim within-window drift, thermal hardware qualification, a Doppler leakage
spectrum or full SAR image formation. The no-drift/frozen comparison exposes
finite estimation noise rather than hiding it with output phase fitting.

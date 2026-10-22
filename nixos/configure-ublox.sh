# Executed before gpsd opens the receiver; all changes are RAM-only.
gps_device="${1:-/dev/gps0}"

ubx() {
  ubxtool -P 14 -f "$gps_device" -w 0.5 "$@"
}

version=$(ubx -p MON-VER -v 2)
grep -q 'hwVersion 00070000' <<< "$version"
grep -qE 'PROTVER[ =]+14[.]00' <<< "$version"

# Enable binary navigation before disabling NMEA. Keep GPS/QZSS rather than
# GLONASS-only mode: this generation supports 10 Hz GPS, but only 1 Hz GLONASS.
# CFG-GNSS for this receiver's 22 tracking channels: GPS, SBAS and QZSS on;
# GLONASS off. Use the version-14 layout rather than newer VALSET keys.
ubx -c 06,3e,00,16,16,04,00,04,ff,00,01,00,00,00,01,01,03,00,01,00,00,00,05,00,03,00,01,00,00,00,06,08,ff,00,00,00,00,00 >/dev/null
ubx -e BINARY -d NMEA -p MODEL,8 -p CFG-RATE,100,1 >/dev/null

# CFG-RXM: continuous tracking. CFG-NAV5 mask 0x0040: static hold disabled;
# other navigation thresholds are retained. Model 8 is airborne <4g.
ubx -c 06,11,08,00 -c 06,24,40,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00 >/dev/null

# NAV-PVT each navigation cycle. Satellite/status and explicit UTC/GPS time
# messages once per second avoid unnecessary traffic at the 10 Hz fix rate.
ubx -p CFG-MSG,1,7,1 -p CFG-MSG,1,48,10 \
    -p CFG-MSG,1,33,10 -p CFG-MSG,1,32,10 \
    -p CFG-MSG,1,6,0 -p CFG-MSG,1,4,10 >/dev/null

# CFG-PRT, USB port 3: allow UBX input and UBX output only. USB baud is not
# a physical UART baud rate; the native USB transport remains full speed.
ubx -c 06,00,03,00,00,00,00,00,00,00,00,00,00,00,01,00,01,00,00,00,00,00 >/dev/null

# ACKs alone do not prove a setting took effect. Poll and verify readback.
state=$(ubxtool -P 14 -f "$gps_device" -w 1 -v 2 \
  -p CFG-RATE -p CFG-NAV5 -p CFG-PRT,3 -p CFG-RXM)
grep -qE 'measRate 100 navRate 1 timeRef' <<< "$state"
grep -qE 'dynModel 8 fixmode' <<< "$state"
grep -qE 'staticHoldThresh 0 ' <<< "$state"
grep -qE 'inProtoMask 0x0*1 outProtoMask 0x0*1' <<< "$state"
grep -qE 'lpMode 0 ' <<< "$state"
printf '%s\n' 'u-blox 7 verified: UBX USB, 10 Hz, airborne <4g, static hold off, continuous tracking.'

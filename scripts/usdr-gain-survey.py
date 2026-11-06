#!/usr/bin/env python3
"""Finite RX-only uSDR gain survey using the deployed native DMA API.

Saves calibrated-in-digital-units PSDs and ADC statistics. Live-antenna guard
power includes interference and distortion; it is not a noise-figure measurement.
No transmit, flash, clock-register overrides or external RF switching is used.
"""
import argparse
import ctypes as C
import fcntl
import json
import os
from pathlib import Path
import random
import time

import numpy as np
from scipy.ndimage import median_filter
from scipy.signal import find_peaks


class Block(C.Structure):
    _fields_ = [("data", C.c_void_p), ("bytes", C.c_size_t), ("samples", C.c_uint),
                ("format", C.c_uint), ("timestamp", C.c_uint64), ("lost", C.c_uint64),
                ("token", C.c_uint64), ("flags", C.c_uint)]


class Receiver:
    def __init__(self, library, rate, frequency, bandwidth):
        self.lib = C.CDLL(library)
        ptr, u64 = C.c_void_p, C.c_uint64
        signatures = {
            "usdr_dmd_create_string": [C.c_char_p, C.POINTER(ptr)], "usdr_dmd_close": [ptr],
            "usdr_dme_set_uint": [ptr, C.c_char_p, u64],
            "usdr_dme_get_uint": [ptr, C.c_char_p, C.POINTER(u64)],
            "usdr_dme_set_string": [ptr, C.c_char_p, C.c_char_p],
            "usdr_dms_create": [ptr, C.c_char_p, C.c_char_p, u64, C.c_uint, C.POINTER(ptr)],
            "usdr_dms_destroy": [ptr], "usdr_dms_op": [ptr, C.c_int, u64],
            "usdr_dms_sync": [ptr, C.c_char_p, C.c_uint, C.POINTER(ptr)],
            "usdr_dms_acquire": [ptr, C.c_uint, C.POINTER(Block)],
            "usdr_dms_release": [ptr, C.POINTER(Block)],
        }
        for name, types in signatures.items():
            getattr(self.lib, name).argtypes = types
            getattr(self.lib, name).restype = C.c_int
        self.lib.usdrlog_setlevel.argtypes = [C.c_char_p, C.c_uint]
        self.lib.usdrlog_setlevel(None, 2)
        self.device, self.stream, self.started = ptr(), ptr(), False
        try:
            self.check(self.lib.usdr_dmd_create_string(b"bus=pcie,dev=usdr0", C.byref(self.device)), "open")
            self.put("/dm/power/en", 1)
            self.put("/dm/rate/master", rate)
            self.check(self.lib.usdr_dme_set_string(self.device, b"/dm/sdr/0/rx/path", b"EXT"), "path")
            self.put("/dm/sdr/0/rx/frequency", frequency)
            self.put("/dm/sdr/0/rx/bandwidth", bandwidth)
            self.put("/dm/sdr/0/rx/gain/auto", 0)
            self.check(self.lib.usdr_dms_create(self.device, b"/ll/srx/0", b"ci16", 1, 16384, C.byref(self.stream)), "stream creation at minimum gain")
        except Exception:
            self.close()
            raise

    @staticmethod
    def check(value, operation):
        if value:
            raise RuntimeError(f"{operation}: {value} ({os.strerror(abs(value))})")

    def put(self, path, value):
        self.check(self.lib.usdr_dme_set_uint(self.device, path.encode(), int(value)), path)

    def get(self, path):
        value = C.c_uint64()
        self.check(self.lib.usdr_dme_get_uint(self.device, path.encode(), C.byref(value)), path)
        return value.value

    def snapshot(self):
        registers = {}
        for reg in [0x54, 0x55, 0x64, 0x65, 0x68, 0x75, 0x76, 0x7c, 0x7d]:
            self.put("/debug/hw/lms6002d/0/reg", reg << 8)
            registers[f"{reg:02x}"] = self.get("/debug/hw/lms6002d/0/reg") & 255
        return {"applied": {key: self.get(key) for key in [
            "/dm/rate/master", "/dm/rate/adc", "/dm/sdr/0/rx/frequency",
            "/dm/sdr/0/rx/frequency/lob", "/dm/sdr/0/rx/bandwidth", "/dm/sdr/0/rx/gain/auto"]},
            "lms_registers": registers}

    def gain(self, case):
        if "auto" in case:
            self.put("/dm/sdr/0/rx/gain/auto", case["auto"])
        else:
            self.put("/dm/sdr/0/rx/gain/lna", case["lna"])
            self.put("/dm/sdr/0/rx/gain/vga", case["vga1"])
            self.put("/dm/sdr/0/rx/gain/vga2a", case["vga2a"])
            self.put("/dm/sdr/0/rx/gain/vga2b", 0)

    def capture(self, blocks):
        result = np.empty((blocks * 16384, 2), dtype="<i2")
        offset, lost, first, last, discontinuities = 0, 0, None, None, 0
        self.check(self.lib.usdr_dms_sync(self.device, b"off", 1, C.byref(self.stream)), "hold sync")
        self.started = True
        self.check(self.lib.usdr_dms_op(self.stream, 0, 0), "start")
        self.check(self.lib.usdr_dms_sync(self.device, b"none", 1, C.byref(self.stream)), "release sync")
        try:
            for index in range(blocks + 64):
                block = Block()
                self.check(self.lib.usdr_dms_acquire(self.stream, 1000, C.byref(block)), "acquire")
                try:
                    if block.format != 1 or block.bytes != block.samples * 4 or block.samples != 16384:
                        raise RuntimeError("Unexpected native DMA layout")
                    if index >= 64:
                        if first is None:
                            first = block.timestamp
                        if last is not None and block.timestamp != last + block.lost:
                            discontinuities += 1
                        last = block.timestamp + block.samples
                        lost += block.lost
                        C.memmove(result.ctypes.data + offset * 4, block.data, block.bytes)
                        offset += block.samples
                finally:
                    self.check(self.lib.usdr_dms_release(self.stream, C.byref(block)), "release")
        finally:
            self.check(self.lib.usdr_dms_op(self.stream, 1, 0), "stop")
            self.started = False
        return result, {"samples": offset, "lost": lost, "discontinuities": discontinuities,
                        "first_tick": first, "end_tick": last}

    def close(self):
        if self.started:
            self.lib.usdr_dms_op(self.stream, 1, 0)
        if self.stream:
            self.lib.usdr_dms_destroy(self.stream)
            self.stream = C.c_void_p()
        if self.device:
            self.lib.usdr_dmd_close(self.device)
            self.device = C.c_void_p()


def analyze(raw, rate, centre):
    values = raw.astype(np.float64) / 32768
    z = values[:, 0] + 1j * values[:, 1]
    dc = z.mean()
    z -= dc
    power = np.mean(np.abs(z) ** 2)
    circularity = np.mean(z * z) / power
    n = 16384
    window = np.hanning(n)
    periods = z[:len(z) // n * n].reshape(-1, n)
    psd = np.fft.fftshift(np.mean(np.abs(np.fft.fft(periods * window, axis=1)) ** 2, axis=0)) / (rate * np.sum(window ** 2))
    frequency = centre + np.fft.fftshift(np.fft.fftfreq(n, 1 / rate))
    db = 10 * np.log10(psd + 1e-30)
    channel_power = {}
    for label, carrier in [("Seven", 177.5e6), ("SBS", 184.5e6), ("Nine", 191.625e6)]:
        mask = (np.abs(frequency - carrier) < 3.3e6) & (np.abs(frequency - centre) > 50e3)
        channel_power[label] = float(10 * np.log10(np.sum(psd[mask]) * rate / n + 1e-30))
    guard = ((frequency > 180.90e6) & (frequency < 181.10e6)) | ((frequency > 187.96e6) & (frequency < 188.16e6))
    excess = db - median_filter(db, size=31, mode="nearest")
    peaks, _ = find_peaks(excess, height=12, distance=8)
    peaks = sorted(peaks, key=lambda i: db[i], reverse=True)[:24]
    info = {"rms_dbfs": float(10 * np.log10(power)),
            "iq_circularity": [float(circularity.real), float(circularity.imag)],
            "iq_component_rms_dbfs": [float(10 * np.log10(np.var(values[:, k]))) for k in range(2)],
            "peak_component_dbfs": float(20 * np.log10(np.max(np.abs(values)) + 1e-30)),
            "component_clip_fraction": float(np.mean(np.abs(raw.astype(np.int32)) >= 32752)),
            "sample_clip_fraction": float(np.mean(np.any(np.abs(raw.astype(np.int32)) >= 32752, axis=1))),
            "component_min_max": [int(raw.min()), int(raw.max())],
            "dc_iq_dbfs": [float(20 * np.log10(abs(value) + 1e-30)) for value in [dc.real, dc.imag]],
            "crest_db": float(10 * np.log10(np.max(np.abs(z) ** 2) / power)),
            "channel_power_dbfs": channel_power,
            "guard_psd_dbfs_hz": float(np.median(db[guard])),
            "guard_to_channel_db_hz": float(np.median(db[guard]) - np.mean(list(channel_power.values()))),
            "strong_narrow_features": [{"rf_hz": float(frequency[i]), "psd_dbfs_hz": float(db[i]),
                                        "local_excess_db": float(excess[i])} for i in peaks]}
    return info, frequency, psd


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--rate", type=int, default=32000000)
    p.add_argument("--frequency", type=int, default=184625000)
    p.add_argument("--bandwidth", type=int, default=28000000)
    p.add_argument("--blocks", type=int, default=256)
    p.add_argument("--repeats", type=int, default=2)
    p.add_argument("--case-json", help='A single case, e.g. {"auto":0} or {"lna":0,"vga1":8,"vga2a":0}')
    p.add_argument("--retain-samples", action="store_true", help="Retain finite native CI16 records for independent analysis")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    lock = os.open("/run/lock/pl64-radio.lock", os.O_RDWR | os.O_CLOEXEC)
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    receiver = None
    report = {"requested": vars(a) | {"out": str(a.out)}, "measurements": [],
              "scope": "Live antenna. Guard power includes external noise, interference and receiver distortion. No absolute NF, IIP3 or calibrated alias-stopband claim."}
    try:
        receiver = Receiver(a.library, a.rate, a.frequency, a.bandwidth)
        cases = ([json.loads(a.case_json)] if a.case_json else
                 [{"auto": v} for v in [0, 3, 5, 6, 9, 12, 15, 18, 24]] +
                 [{"lna": 0, "vga1": v, "vga2a": 0} for v in [5, 8, 11, 14, 17, 20, 23, 29]] +
                 [{"lna": 0, "vga1": 5, "vga2a": v} for v in [3, 6, 9, 12, 18, 24]] +
                 [{"lna": 6, "vga1": v, "vga2a": 0} for v in [5, 8, 11]])
        randomizer = random.Random(20261106)
        for repeat in range(a.repeats):
            order = list(cases)
            if repeat:
                randomizer.shuffle(order)
            for case in order:
                receiver.gain(case)
                raw, transport = receiver.capture(a.blocks)
                state = receiver.snapshot()
                applied = state["applied"]
                metrics, frequency, psd = analyze(raw, applied["/dm/rate/master"], applied["/dm/sdr/0/rx/frequency"])
                item = {"index": len(report["measurements"]), "repeat": repeat, "case": case,
                        "utc_ns": time.time_ns(), "state": state, "transport": transport, "metrics": metrics}
                report["measurements"].append(item)
                if a.retain_samples:
                    raw.tofile(a.out / f"raw-{item['index']:03d}.ci16")
                np.savez_compressed(a.out / f"spectrum-{item['index']:03d}.npz", frequency_hz=frequency, psd=psd)
                (a.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                print(json.dumps({"case": case, "repeat": repeat, "rms_dbfs": metrics["rms_dbfs"],
                                  "peak_dbfs": metrics["peak_component_dbfs"],
                                  "clipped": metrics["sample_clip_fraction"], "guard": metrics["guard_to_channel_db_hz"],
                                  "lost": transport["lost"]}), flush=True)
    finally:
        if receiver:
            receiver.close()
        os.close(lock)


if __name__ == "__main__":
    main()

"""Package the two driving runs and an OSM extract into one offline HTML file.

Requires numpy/scipy and the existing decoded MCAPs. The optional 20 Hz sidecar
comes from audit.cpp; no estimate is recomputed or 'repaired' by this exporter.
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation

RUNS = [
    ("middle", "session-1794105395676964619-682", "Run 2 · 13:36–13:43"),
    ("long", "session-1794106036529468891-682", "Run 3 · 13:47–14:00"),
]
ORIGIN = np.deg2rad([-37.847, 144.963])
RADIUS = 6378137.
STAGES = ["Built-in test", "Coarse alignment", "Fine alignment", "Navigating", "Degraded"]


def project(lat, lon):
    """Locally scaled Mercator; east/north in approximately ground metres."""
    scale = RADIUS * np.cos(ORIGIN[0])
    return (scale * (lon - ORIGIN[1]),
            scale * (np.arcsinh(np.tan(lat)) - np.arcsinh(np.tan(ORIGIN[0]))))


def ned_to_ecef(lat, lon):
    s, c, sl, cl = np.sin(lat), np.cos(lat), np.sin(lon), np.cos(lon)
    z = np.zeros_like(lat)
    return np.stack([-s*cl, -sl, -c*cl, -s*sl, cl, -c*sl, c, z, -s], axis=-1).reshape(-1, 3, 3)


def ecef_to_geodetic(p):
    # Same WGS84 conversion as production model.cpp; vectorised for the export.
    a, e2 = 6378137., 6.6943799901413165e-3
    rho = np.hypot(p[:, 0], p[:, 1])
    lat = np.arctan2(p[:, 2], rho * (1 - e2))
    for _ in range(8):
        n = a / np.sqrt(1 - e2 * np.sin(lat)**2)
        h = rho / np.cos(lat) - n
        lat = np.arctan2(p[:, 2], rho * (1 - e2 * n / (n + h)))
    return lat, np.arctan2(p[:, 1], p[:, 0]), h


def geographic(state, origin, basis):
    ecef = origin + np.einsum("nij,nj->ni", basis, state[:, :3])
    lat, lon, alt = ecef_to_geodetic(ecef)
    # Filter attitude maps packet-accel axes into a fixed NED tangent frame.
    # Rotate to *current local* NED before plotting pitch/roll/true heading.
    local = np.swapaxes(ned_to_ecef(lat, lon), 1, 2) @ basis
    q = Rotation.from_matrix(local @ Rotation.from_quat(state[:, 6:10]).as_matrix()).as_quat()
    velocity = np.einsum("nij,nj->ni", local, state[:, 3:6])
    x, y = project(lat, lon)
    return x, y, np.rad2deg(lat), np.rad2deg(lon), alt, q, velocity


def finite(value, digits=3):
    return round(float(value), digits) if np.isfinite(value) else None


def state_rows(time, state, origin, basis, stage, hs, ts, age, reason, segment,
               observed, t0):
    n = len(time)
    valid = np.isfinite(origin).all(axis=1) & (stage != 0)
    geo = np.full((n, 12), np.nan)
    if valid.any():
        x, y, lat, lon, alt, q, v = geographic(state[valid], origin[valid], basis[valid])
        geo[valid] = np.column_stack([x, y, lat, lon, alt, q, v])
    rows = []
    for i in range(n):
        discontinuity = (i == 0 or time[i] - time[i-1] > .2 or segment[i] != segment[i-1]
                         or stage[i] != stage[i-1] or valid[i] != valid[i-1]
                         or (valid[i] and np.linalg.norm(origin[i] - origin[i-1]) > .01))
        # t, E, N, lat, lon, alt, qxyzw, vNED, stage, sigmas, age,
        # reason index, recording segment, discontinuity, heading_observed.
        row = [finite(time[i] - t0, 6)]
        row += [finite(v, 8 if j in (2, 3) else 7 if 5 <= j < 9 else 3)
                for j, v in enumerate(geo[i])]
        row += [int(stage[i]), finite(hs[i]), finite(ts[i]), finite(age[i]),
                int(reason[i]), int(segment[i]), int(discontinuity), int(observed[i])]
        rows.append(row)
    assert all(a[0] < b[0] for a, b in zip(rows, rows[1:])), "Non-monotonic state epochs"
    return rows


def candidate(path, t0, reasons):
    raw = [json.loads(line) for line in path.read_text().splitlines()]
    def field(key):
        return np.array([r[key] for r in raw])
    rid = [intern(reasons, r["reason"]) for r in raw]
    origin = np.array([r.get("earth_origin", [np.nan]*3) for r in raw])
    basis = np.array([r.get("earth_to_ecef", [np.nan]*9) for r in raw]).reshape(-1, 3, 3)
    return state_rows(field("time"), field("state"), origin, basis, field("stage"),
                      field("heading_sigma_deg"), field("tilt_sigma_deg"),
                      field("gnss_age"), rid, field("segment"), field("heading_observed"), t0)


def intern(items, value):
    if value not in items:
        items.append(value)
    return items.index(value)


def recorded(data, meta, t0, reasons):
    s = data["state"]
    # Retain 20 Hz, the last sample, every flag transition and gap boundary.
    bins = np.floor((s[:, 0] - t0) * 20).astype(np.int64)
    keep = np.r_[True, (np.diff(bins) != 0) | (np.diff(s[:, 3]) != 0) |
                 (np.diff(s[:, 48]) != 0) | (np.diff(s[:, 0]) > .2)]
    # Keep the sample immediately before a gap as well.
    keep[:-1] |= np.diff(s[:, 0]) > .2
    keep[-1] = True
    s = s[keep]
    flags = s[:, 3].astype(int)
    q = Rotation.from_quat(s[:, 14:18])
    down = q.inv().apply(np.tile([0., 0., 1.], (len(s), 1)))
    cov = s[:, 36:45].reshape(-1, 3, 3)
    hvar = np.einsum("ni,nij,nj->n", down, cov, down)
    hs = np.rad2deg(np.sqrt(np.maximum(0, hvar)))
    ts = np.rad2deg(np.sqrt(np.maximum(0, np.trace(cov, axis1=1, axis2=2) - hvar)))
    reason_map = {v: intern(reasons, k.split(": ", 1)[-1]) for k, v in meta["reasons"].items()}
    return state_rows(s[:, 0], s[:, 8:24], s[:, 24:27], s[:, 27:36].reshape(-1, 3, 3),
                      flags & 255, hs, ts, s[:, 4], [reason_map[int(v)] for v in s[:, 7]],
                      s[:, 48], (flags & 256) != 0, t0)


def map_features(path):
    osm = json.loads(path.read_text())
    result = []
    for way in osm["elements"]:
        if way["type"] != "way" or "geometry" not in way:
            continue
        tags = way.get("tags", {})
        if "highway" in tags:
            h = tags["highway"]
            kind = ("major" if h in ("motorway", "trunk", "primary", "secondary", "tertiary") else
                    "path" if h in ("footway", "path", "cycleway", "steps", "pedestrian", "bridleway") else
                    "minor" if h in ("service", "track", "living_street") else "road")
        elif "building" in tags:
            kind = "building"
        elif tags.get("natural") == "coastline":
            kind = "coast"
        elif tags.get("natural") == "water":
            kind = "water"
        elif "waterway" in tags:
            kind = "waterway"
        elif "railway" in tags:
            kind = "rail"
        elif (tags.get("leisure") in ("park", "garden", "pitch", "nature_reserve") or
              tags.get("landuse") in ("grass", "forest", "meadow", "recreation_ground")):
            kind = "park"
        else:
            continue
        points = [p for p in way["geometry"] if "lat" in p and "lon" in p]
        if len(points) < 2:
            continue
        x, y = project(np.deg2rad([p["lat"] for p in points]),
                       np.deg2rad([p["lon"] for p in points]))
        coords = np.round(np.column_stack([x, y]), 1).ravel().tolist()
        box = np.round([min(x), min(y), max(x), max(y)], 1).tolist()
        holes = []
        for hole in way.get("holes", []):
            hx, hy = project(np.deg2rad([p["lat"] for p in hole]), np.deg2rad([p["lon"] for p in hole]))
            holes.append(np.round(np.column_stack([hx, hy]), 1).ravel().tolist())
        result.append([kind, tags.get("name", ""), coords, box, holes])
    return {"features": result, "attribution": "© OpenStreetMap contributors",
            "license": "https://www.openstreetmap.org/copyright",
            "snapshot": osm.get("osm3s", {}).get("timestamp_osm_base", "unknown"),
            "source": json.loads(path.with_name("streets.source.json").read_text()),
            "projection_origin_deg": np.rad2deg(ORIGIN).tolist()}


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def build(root, output):
    runs = []
    for key, name, label in RUNS:
        npz = root / "decoded" / (name + ".npz")
        d = np.load(npz)
        meta = json.loads(npz.with_suffix(".json").read_text())
        t0, t1 = d["imu"][[0, -1], 0]
        reasons = []
        replay = candidate(root / "playback" / (key + ".states.jsonl"), t0, reasons)
        live = recorded(d, meta, t0, reasons)
        g = d["gps"]
        x, y = project(g[:, 3], g[:, 4])
        valid = (g[:, 15].astype(int) & 1) != 0
        gps = [[finite(r[0]-t0, 6), finite(x[i]) if valid[i] else None,
                finite(y[i]) if valid[i] else None, *[finite(v) for v in r[6:9]],
                finite(np.hypot(r[9], r[10])), int(valid[i]), int(r[17])]
               for i, r in enumerate(g) if t0 <= r[0] <= t1]
        imu = d["imu"]
        gaps = [[finite(imu[i, 0]-t0, 6), finite(imu[i+1, 0]-t0, 6)]
                for i in np.flatnonzero(np.diff(imu[:, 0]) > .2)]
        config = root / "configs" / (name + "-gyro_frame_hist60_calibrated.json")
        runs.append({"id": key, "name": name, "label": label, "duration": float(t1-t0),
                     "utc_start": float(imu[0, 2]), "replay": replay, "recorded": live,
                     "gps": gps, "reasons": reasons, "gaps": gaps,
                     "bounds": [float(min(x[valid])), float(min(y[valid])),
                                float(max(x[valid])), float(max(y[valid]))],
                     "source_sha256": sha256(npz), "config_sha256": sha256(config),
                     "config": json.loads(config.read_text())})
    payload = {"runs": runs, "map": map_features(root / "playback" / "streets.osm.json"),
               "stages": STAGES, "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
               "variant": "gyro_frame_hist60_calibrated", "display_rate_hz": 20}
    template = Path(__file__).with_name("playback.html").read_text()
    data = json.dumps(payload, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    # Safe even if a map feature or sensor detail contains HTML/script delimiters.
    data = data.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    output.write_text(template.replace("/*__PLAYBACK_DATA__*/", data))
    print(json.dumps({"output": str(output), "bytes": output.stat().st_size,
                      "map_features": len(payload["map"]["features"]),
                      "runs": [{"name": r["label"], "duration": r["duration"],
                                "replay_states": len(r["replay"]), "live_states": len(r["recorded"]),
                                "gaps": r["gaps"]} for r in runs]}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("data", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    build(args.data.resolve(), args.output or args.data / "navigation-playback.html")

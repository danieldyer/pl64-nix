"""Cache an OSM vector map for the driving runs, using bounded read-only requests.

No trajectories are uploaded: only geographic bounding boxes reach the public
OSM API. Downloads are cached, use at most two connections, and never happen in
the viewer. The resulting embedded extract is attributed and licensed as ODbL.
"""
import argparse
import concurrent.futures
import datetime as dt
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def rings(parts):
    """Join OSM multipolygon members without inventing missing boundary edges."""
    pending = [list(p) for p in parts]
    result = []
    while pending:
        chain = pending.pop()
        while chain[-1] != chain[0]:
            match = next((i for i, part in enumerate(pending)
                          if part[0] == chain[-1] or part[-1] == chain[-1]), None)
            if match is None:
                break
            part = pending.pop(match)
            if part[-1] == chain[-1]:
                part.reverse()
            chain.extend(part[1:])
        if len(chain) >= 4 and chain[-1] == chain[0]:
            result.append(chain)
    return result


def inside(point, ring):
    x, y = point["lon"], point["lat"]
    odd = False
    for a, b in zip(ring, ring[1:]):
        if (a["lat"] > y) != (b["lat"] > y):
            crossing = a["lon"] + (y-a["lat"])*(b["lon"]-a["lon"])/(b["lat"]-a["lat"])
            if x < crossing:
                odd = not odd
    return odd


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("data", type=Path)
    args = p.parse_args()
    root = args.data / "playback"
    cache = root / "osm-cache"
    cache.mkdir(parents=True, exist_ok=True)
    west, south, east, north = 144.932, -37.867, 144.994, -37.826
    boxes = [(west+(east-west)*x/4, south+(north-south)*y/4,
              west+(east-west)*(x+1)/4, south+(north-south)*(y+1)/4)
             for x in range(4) for y in range(4)]

    def fetch(item):
        i, box = item
        path = cache / (str(i) + ".osm")
        if not path.exists():
            temporary = path.with_suffix(".download")
            url = "https://api.openstreetmap.org/api/0.6/map?bbox=" + ",".join(f"{v:.7f}" for v in box)
            subprocess.run(["curl", "--fail", "--silent", "--show-error", "--max-time", "90",
                            "--user-agent", "PL64-offline-navigation-review/1.0",
                            url, "-o", str(temporary)], check=True)
            ET.parse(temporary)  # Never cache a proxy error page as map data.
            temporary.replace(path)
        print("Map tile", i+1, "/", len(boxes), path.stat().st_size, "bytes", flush=True)
        return path

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        paths = list(pool.map(fetch, enumerate(boxes)))
    nodes, ways, relations = {}, {}, {}
    for path in paths:
        for element in ET.parse(path).getroot():
            if element.tag == "node":
                nodes[element.attrib["id"]] = {"lat": float(element.attrib["lat"]),
                                               "lon": float(element.attrib["lon"])}
            elif element.tag == "way":
                tags = {e.attrib["k"]: e.attrib["v"] for e in element if e.tag == "tag"}
                ways[element.attrib["id"]] = (tags, [e.attrib["ref"] for e in element if e.tag == "nd"])
            elif element.tag == "relation":
                tags = {e.attrib["k"]: e.attrib["v"] for e in element if e.tag == "tag"}
                if tags.get("type") == "multipolygon" and tags.get("natural") == "water":
                    relations[element.attrib["id"]] = (tags, [e.attrib for e in element if e.tag == "member"])
    elements = []
    for ident, (tags, refs) in ways.items():
        if not any(k in tags for k in ("highway", "building", "natural", "waterway", "railway",
                                      "leisure", "landuse")):
            continue
        if not all(ref in nodes for ref in refs):
            raise RuntimeError("Missing geometry for way " + ident)
        elements.append({"type": "way", "id": int(ident), "tags": tags,
                         "geometry": [nodes[ref] for ref in refs]})
    for ident, (tags, members) in relations.items():
        members = [m for m in members if m["type"] == "way"]
        if not all(m["ref"] in ways for m in members):
            continue  # A relation extending outside the extract is not a polygon.
        outer = rings([ways[m["ref"]][1] for m in members if m["role"] == "outer"])
        inner = rings([ways[m["ref"]][1] for m in members if m["role"] == "inner"])
        for ring in outer:
            boundary = [nodes[n] for n in ring]
            holes = [[nodes[n] for n in hole] for hole in inner if inside(nodes[hole[0]], boundary)]
            elements.append({"type": "way", "id": -int(ident), "tags": tags,
                             "geometry": boundary, "holes": holes})
    stamp = dt.datetime.fromtimestamp(min(p.stat().st_mtime for p in paths), dt.timezone.utc).isoformat()
    source = {"url": "https://api.openstreetmap.org/api/0.6/map", "bounds": [west, south, east, north],
              "tiles": boxes, "retrieved_utc": stamp, "copyright": "OpenStreetMap contributors",
              "license": "https://opendatacommons.org/licenses/odbl/1-0/"}
    (root / "streets.source.json").write_text(json.dumps(source, indent=2))
    (root / "streets.osm.json").write_text(json.dumps({"elements": elements,
                                                       "osm3s": {"timestamp_osm_base": stamp}}))
    print("Wrote", len(elements), "vector features", flush=True)


if __name__ == "__main__":
    main()

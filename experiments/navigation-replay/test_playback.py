"""Offline browser acceptance checks for a generated navigation-playback.html.

Requires Python playwright and an installed Chrome/Chromium. Produces screenshots
and a small JSON report beside the HTML, without touching the recording sources.
"""
import argparse
import json
import math
from pathlib import Path
import time

from playwright.sync_api import expect, sync_playwright


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("html", type=Path)
    p.add_argument("--chrome", default="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
    args = p.parse_args()
    dest = args.html.parent / "playback" / "browser-check"
    dest.mkdir(parents=True, exist_ok=True)
    errors, requests, report = [], [], {}
    with sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=args.chrome, headless=True)
        context = browser.new_context(viewport={"width": 1440, "height": 980}, device_scale_factor=1)
        context.set_offline(True)
        page = context.new_page()
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.on("request", lambda request: requests.append(request.url))
        page.goto(args.html.resolve().as_uri())
        expect(page.locator("#reason")).not_to_have_text("Loading recording…")
        assert page.locator("#run option").count() == 2
        assert page.evaluate("DATA.map.features.length") > 100
        assert page.evaluate("DATA.runs.every(r=>r.recorded.every(s=>s[13]!==3))")
        # Known attitude: identity packet frame with -Z boresight is straight up;
        # pitch +90 degrees, no meaningful azimuth. Both vectors must be hidden.
        angles = page.evaluate("""() => {
          const s=run.replay.find(s=>s[13]===3).slice();
          s.splice(6,4,0,0,0,1);const a=attitude(s);
          // q = +90 degrees about X: packet +Y down, -Z east.
          s.splice(6,4,Math.SQRT1_2,0,0,Math.SQRT1_2);const b=attitude(s);
          return {vertical:a,cardinal:b};
        }""")
        assert math.isclose(angles["vertical"]["pitch"], 90, abs_tol=1e-9)
        assert not angles["vertical"]["resolved"]
        assert math.isclose(angles["cardinal"]["heading"], 90, abs_tol=1e-9)
        assert abs(angles["cardinal"]["pitch"]) < 1e-9
        assert abs(angles["cardinal"]["roll"]) < 1e-9
        # Analytic yaw/pitch/roll fixture, with the packet mounting undone.
        sr, cr = math.sin(math.radians(10)), math.cos(math.radians(10))
        sp, cp = math.sin(math.radians(15)), math.cos(math.radians(15))
        sy, cy = math.sin(math.radians(61.5)), math.cos(math.radians(61.5))
        x, y, z, w = sr*cp*cy-cr*sp*sy, cr*sp*cy+sr*cp*sy, cr*cp*sy-sr*sp*cy, cr*cp*cy+sr*sp*sy
        q = [.5*(w+x-y+z), .5*(-w+y+z+x), .5*(-w+z-x-y), .5*(w-x+y+z)]
        fixture = page.evaluate("""q=>{const s=run.replay.find(s=>s[13]===3).slice();
          s.splice(6,4,...q);return attitude(s);}""", q)
        for key, expected in [("heading", 123), ("pitch", 30), ("roll", 20)]:
            assert math.isclose(fixture[key], expected, abs_tol=1e-9), fixture
        report["cardinal_attitudes"] = "passed"
        for index in range(2):
            page.select_option("#run", str(index))
            page.wait_for_timeout(80)
            page.locator("#first-nav").click()
            page.wait_for_timeout(100)
            assert page.locator("#state").inner_text() == "Navigating"
            assert page.locator("#heading").inner_text() != "—"
            start = page.evaluate("time")
            pose = page.evaluate("({t:time,attitude:attitude(sampleState(run[mode],time)),state:sampleState(run[mode],time),gps:sampleGps(time)})")
            page.select_option("#mount", "1")
            page.wait_for_timeout(60)
            opposite = page.evaluate("attitude(sampleState(run[mode],time))")
            delta = (opposite["heading"] - pose["attitude"]["heading"]) % 360
            assert math.isclose(delta, 180, abs_tol=1e-9)
            assert math.isclose(opposite["pitch"], -pose["attitude"]["pitch"], abs_tol=1e-9)
            page.select_option("#mount", "-1")
            page.locator("#follow").click()
            page.locator("#zoom-in").click()
            page.locator("#show-other").check()
            page.wait_for_timeout(100)
            page.screenshot(path=str(dest / f"run-{index+2}.png"))
            page.locator("#play").click()
            wall = time.monotonic()
            page.wait_for_timeout(2100)
            page.locator("#play").click()
            elapsed = time.monotonic() - wall
            advanced = page.evaluate("time") - start
            assert abs(advanced - elapsed) < .45, (advanced, elapsed)
            # A paused scrub is stable and renders the new state immediately.
            page.locator("#scrub").evaluate("e=>{e.value='30';e.dispatchEvent(new Event('input',{bubbles:true}));}")
            page.wait_for_timeout(100)
            assert page.evaluate("time") == 30
            assert page.locator("#heading").inner_text() == "—"
            assert "unresolved" in page.locator("#map-note").inner_text()
            page.select_option("#estimate", "recorded")
            page.wait_for_timeout(100)
            assert page.locator("#first-nav").is_disabled()
            page.select_option("#estimate", "replay")
            page.evaluate("document.activeElement.blur()")
            page.keyboard.press("ArrowRight")
            assert page.evaluate("time") == 31
            page.keyboard.press("Shift+ArrowLeft")
            assert page.evaluate("time") == 21
            page.keyboard.press("End")
            assert page.evaluate("time===run.duration")
            page.locator("#play").click()
            page.wait_for_timeout(120)
            assert page.evaluate("time") < 1
            page.locator("#play").click()
            report[f"run_{index+2}"] = {"first_navigating_s": start, "pose": pose,
                                        "realtime_wall_s": elapsed, "playback_advance_s": advanced}
        # The long run's missing IMU interval must blank attitude, not connect it.
        gap = page.evaluate("run.gaps[0]")
        page.evaluate("t=>seek(t)", sum(gap)/2)
        page.wait_for_timeout(80)
        assert page.locator("#state").inner_text() == "Recording gap"
        assert page.locator("#pitch").inner_text() == "—"
        assert page.evaluate("sampleState(run.replay,time)") is None
        report["recording_gap"] = gap
        # No interpolation across stage changes, even if only 5 ms apart.
        assert page.evaluate("""() => {
          const r=run.replay,i=r.findIndex((s,i)=>i>0&&s[13]===3&&r[i-1][13]!==3);
          return JSON.stringify(sampleState(r,(r[i-1][0]+r[i][0])/2))===JSON.stringify(r[i-1]);
        }""")
        page.locator("#first-nav").click()
        page.locator("#fit").click()
        page.set_viewport_size({"width": 430, "height": 932})
        page.wait_for_timeout(150)
        assert page.evaluate("document.documentElement.scrollWidth<=innerWidth")
        page.screenshot(path=str(dest / "mobile.png"), full_page=True)
        page.set_viewport_size({"width": 1280, "height": 853})
        page.wait_for_timeout(150)
        page.locator("#fit").click()
        page.screenshot(path=str(dest / "150-percent-desktop.png"))
        report["map_features"] = page.evaluate("DATA.map.features.length")
        report["external_requests"] = [r for r in requests if not r.startswith("file:")]
        report["errors"] = errors
        assert not errors, errors
        assert not report["external_requests"], report["external_requests"]
        browser.close()
    (dest / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"result": "passed", "report": str(dest / 'report.json')}, indent=2))


if __name__ == "__main__":
    main()

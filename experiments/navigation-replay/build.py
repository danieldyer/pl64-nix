"""Build a diagnostic copy of the production estimator; never edit cpp/ins."""
import argparse
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p = argparse.ArgumentParser(__doc__)
p.add_argument("output", type=Path)
p.add_argument("--eigen", type=Path, required=True, help="Directory containing Eigen/Core")
p.add_argument("--json", type=Path, required=True, help="Directory containing nlohmann/json.hpp")
p.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
p.add_argument("--unmodified", action="store_true", help="Build an uninstrumented parity control")
args = p.parse_args()
here = Path(__file__).resolve().parent
repo = here.parents[1]
out = args.output.resolve()
source = out / "source" / "ins"
if source.exists():
    p.error("Use a fresh output directory: " + str(source))
out.mkdir(parents=True, exist_ok=True)
shutil.copytree(repo / "cpp" / "ins", source)
originals = {f.relative_to(source).as_posix(): f.read_bytes()
             for f in source.rglob("*") if f.is_file()}
if args.unmodified:
    (source / "src" / "analysis.hpp").write_text(
        "#pragma once\n#include <nlohmann/json.hpp>\n"
        "namespace ins::audit { extern double history; void emit(nlohmann::json); }\n")
else:
    subprocess.run([sys.executable, str(here / "instrument.py"), str(source.parent)], check=True)
shutil.copy(here / "audit.cpp", source / "src" / "audit.cpp")
diff = []
for name, content in originals.items():
    changed = (source / name).read_bytes()
    if changed != content:
        diff.extend(difflib.unified_diff(content.decode().splitlines(True),
                                       changed.decode().splitlines(True), name, name))
(out / "instrumentation.diff").write_text("".join(diff))
flags = [args.compiler, "-std=c++23", "-O2", "-DNDEBUG",
         "-I" + str(source / "include"), "-I" + str(args.eigen.resolve()),
         "-I" + str(args.json.resolve())]
names = ["model", "ukf", "alignment", "estimator", "config", "audit"]


def compile_one(name):
    result = subprocess.run(flags + ["-c", str(source / "src" / (name + ".cpp")),
                                    "-o", str(out / (name + ".o"))],
                            capture_output=True, text=True)
    (out / (name + ".log")).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("Compilation failed; see " + str(out / (name + ".log")))


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(compile_one, names))
subprocess.run([args.compiler, *[str(out / (name + ".o")) for name in names],
                "-o", str(out / "nav-audit")], check=True)
(out / "manifest.json").write_text(json.dumps({
    "production_source_sha256": {n: hashlib.sha256(b).hexdigest()
                                  for n, b in originals.items()},
    "audit_sha256": hashlib.sha256((here / "audit.cpp").read_bytes()).hexdigest(),
    "compiler": subprocess.check_output([args.compiler, "--version"], text=True),
    "flags": flags,
    "instrumented": not args.unmodified,
}, indent=2))
print(out / "nav-audit")

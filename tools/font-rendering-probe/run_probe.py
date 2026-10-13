import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--probe", required=True, type=Path)
parser.add_argument("--qt", required=True, type=Path)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--cases", nargs="*")
args = parser.parse_args()
qt = args.qt.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
cases = [
    ("editor-native", "editor", "native", 1, False, {}, []),
    ("editor-lcd", "editor", "native", 1, False, {}, ["--quality", "3"]),
    ("editor-no-aa", "editor", "native", 1, False, {}, ["--quality", "1"]),
    ("editor-freetype", "editor", "freetype", 1, False, {}, []),
    ("editor-freetype-legacy", "editor", "freetype", 1, False, {}, ["--legacy-measure"]),
    ("editor-gdi", "editor", "gdi", 1, False, {}, []),
    ("editor-mask-identity", "editor", "native", 1, False, {"MRST_PROBE_MASK":"1"}, []),
    ("editor-mask-uncached", "editor", "native", 1, False,
        {"MRST_PROBE_MASK":"1", "MRST_PROBE_GAMMA":"1.2", "MRST_PROBE_STRENGTH":"1.1"}, []),
    ("editor-mask-cached", "editor", "native", 1, False,
        {"MRST_PROBE_MASK":"1", "MRST_PROBE_GAMMA":"1.2", "MRST_PROBE_STRENGTH":"1.1", "MRST_PROBE_CACHE":"1"}, []),
    ("editor-native-unique", "editor", "native", 1, False, {}, ["--unique-lines"]),
    ("editor-mask-unique", "editor", "native", 1, False,
        {"MRST_PROBE_MASK":"1", "MRST_PROBE_GAMMA":"1.2", "MRST_PROBE_STRENGTH":"1.1", "MRST_PROBE_CACHE":"1"}, ["--unique-lines"]),
    ("editor-native-dark", "editor", "native", 1, True, {}, []),
    ("editor-mask-dark", "editor", "native", 1, True,
        {"MRST_PROBE_MASK":"1", "MRST_PROBE_GAMMA":"1.2", "MRST_PROBE_STRENGTH":"1.1", "MRST_PROBE_CACHE":"1"}, []),
    ("editor-native-150", "editor", "native", 1.5, False, {}, []),
    ("editor-mask-150", "editor", "native", 1.5, False,
        {"MRST_PROBE_MASK":"1", "MRST_PROBE_GAMMA":"1.2", "MRST_PROBE_STRENGTH":"1.1", "MRST_PROBE_CACHE":"1"}, []),
    ("editor-freetype-150", "editor", "freetype", 1.5, False, {}, []),
    ("preview-native", "preview", "native", 1, False, {}, []),
    ("preview-freetype", "preview", "freetype", 1, False, {}, []),
    ("preview-gamma-low", "preview", "native", 1, False, {"EXTRA_CHROMIUM":"--text-gamma=1.0 --text-contrast=0.0"}, []),
    ("preview-gamma-high", "preview", "native", 1, False, {"EXTRA_CHROMIUM":"--text-gamma=2.2 --text-contrast=0.5"}, []),
    ("preview-stroke", "preview", "native", 1, False, {}, ["--css", "stroke"]),
    ("preview-weight", "preview", "native", 1, False, {}, ["--css", "weight"]),
    ("preview-weight-bold", "preview", "native", 1, False, {}, ["--css", "weight-bold"]),
    ("preview-smoothing", "preview", "native", 1, False, {}, ["--css", "smoothing"]),
    ("preview-native-dark", "preview", "native", 1, True, {}, []),
    ("preview-gamma-dark", "preview", "native", 1, True, {"EXTRA_CHROMIUM":"--text-gamma=2.2 --text-contrast=0.5"}, []),
    ("preview-stroke-dark", "preview", "native", 1, True, {}, ["--css", "stroke"]),
    ("preview-native-150", "preview", "native", 1.5, False, {}, []),
    ("preview-gamma-150", "preview", "native", 1.5, False, {"EXTRA_CHROMIUM":"--text-gamma=2.2 --text-contrast=0.5"}, []),
]
# QT_SCALE_FACTOR multiplies monitor scaling. Normalize effective DPR per case.
calibration_env = os.environ.copy()
calibration_env["PATH"] = str(qt / "bin") + os.pathsep + calibration_env.get("PATH", "")
calibration_env["QT_QPA_PLATFORM"] = "windows:fontengine=native"
calibration_env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt / "plugins/platforms")
calibration_env["QT_SCALE_FACTOR"] = "1"
for key in list(calibration_env):
    if key.startswith("MRST_PROBE_"):
        calibration_env.pop(key)
subprocess.run([str(args.probe.resolve()), "--output", str(output / "calibration")],
    env=calibration_env, check=True, timeout=45, capture_output=True)
base_dpr = json.loads((output / "calibration/metrics.json").read_text(encoding="utf-8"))["dpr"]
print("Monitor DPR: " + str(base_dpr), flush=True)
records = []
for name, surface, engine, scale, dark, overrides, extra in cases:
    if args.cases and name not in args.cases:
        continue
    destination = output / name
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("MRST_PROBE_"):
            env.pop(key)
    env["PATH"] = str(qt / "bin") + os.pathsep + env.get("PATH", "")
    env["QT_QPA_PLATFORM"] = "windows:fontengine=" + engine
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt / "plugins/platforms")
    env["QT_SCALE_FACTOR"] = str(scale / base_dpr)
    env["QTWEBENGINEPROCESS_PATH"] = str(qt / "bin/QtWebEngineProcess.exe")
    env["QTWEBENGINE_RESOURCES_PATH"] = str(qt / "resources")
    env["QTWEBENGINE_LOCALES_PATH"] = str(qt / "translations/qtwebengine_locales")
    env["QTWEBENGINE_CHROMIUM_FLAGS"] = "--no-sandbox " + overrides.get("EXTRA_CHROMIUM", "")
    env.update({k:v for k,v in overrides.items() if k != "EXTRA_CHROMIUM"})
    command = [str(args.probe.resolve()), "--output", str(destination), "--surface", surface] + extra
    if dark:
        command.append("--dark")
    print(name + ": start", flush=True)
    startup = subprocess.STARTUPINFO() if os.name == "nt" else None
    if startup:
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    try:
        completed = subprocess.run(command, env=env, capture_output=True, timeout=45, startupinfo=startup)
        record = {"name":name, "surface":surface, "exitCode":completed.returncode,
                  "stderr":completed.stderr.decode("utf-8", errors="replace")}
        if completed.returncode == 0:
            record["metrics"] = json.loads((destination / "metrics.json").read_text(encoding="utf-8"))
    except subprocess.TimeoutExpired:
        record = {"name":name, "surface":surface, "error":"timeout"}
    records.append(record)
    print(name + ": " + str(record.get("exitCode", record.get("error"))), flush=True)
(output / "runs.json").write_text(json.dumps(records, indent=2, ensure_ascii=False), encoding="utf-8")
if any(record.get("exitCode") != 0 for record in records):
    raise SystemExit(1)

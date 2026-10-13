import argparse
import base64
import html
import json
import math
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--results", required=True, type=Path)
parser.add_argument("--probe", required=True, type=Path)
parser.add_argument("--qt", required=True, type=Path)
args = parser.parse_args()
root = args.results.resolve()
records = json.loads((root / "runs.json").read_text(encoding="utf-8"))
runs = {r["name"]: r["metrics"] for r in records if r.get("exitCode") == 0}
assert len(runs) == len(records), "Some cases failed"
assert all(m["hitFailures"] == 0 for n,m in runs.items() if n.startswith("editor-"))
assert all(m.get("loaded") and m.get("imageLoaded") for n,m in runs.items() if n.startswith("preview-"))
pairs = [
    ("editor-native", "editor-mask-cached", "편집기 — 감마·농도 보정 / 100%"),
    ("editor-native-unique", "editor-mask-unique", "편집기 — 서로 다른 행 / 100%"),
    ("editor-native-150", "editor-mask-150", "편집기 — 감마·농도 보정 / 150%"),
    ("editor-native-dark", "editor-mask-dark", "편집기 — 다크 테마"),
    ("editor-native", "editor-freetype", "편집기 — FreeType / 100%"),
    ("editor-native-150", "editor-freetype-150", "편집기 — FreeType / 150%"),
    ("editor-native", "editor-lcd", "편집기 — LCD 품질 설정"),
    ("preview-native", "preview-gamma-low", "프리뷰 — gamma 1.0 / contrast 0.0"),
    ("preview-native", "preview-gamma-high", "프리뷰 — gamma 2.2 / contrast 0.5"),
    ("preview-native-150", "preview-gamma-150", "프리뷰 — 감마·대비 / 150%"),
    ("preview-native-dark", "preview-gamma-dark", "프리뷰 — 감마·대비 / 다크 테마"),
    ("preview-native", "preview-stroke", "프리뷰 — CSS text-stroke 0.15px"),
    ("preview-native", "preview-weight", "프리뷰 — font-weight 500"),
    ("preview-native", "preview-weight-bold", "프리뷰 — font-weight 700"),
    ("preview-native", "preview-smoothing", "프리뷰 — font-smoothing"),
    ("preview-native", "preview-freetype", "프리뷰 — Qt FreeType 옵션"),
]
env = os.environ.copy()
env["PATH"] = str(args.qt.resolve() / "bin") + os.pathsep + env.get("PATH", "")
env["QT_QPA_PLATFORM"] = "windows"
def compare(a, b, label, crop=None):
    name = a + "_vs_" + b + ("_assets" if crop else "")
    out = root / "pixel-comparisons" / name
    surface = "editor" if a.startswith("editor-") else "preview"
    command = [str(args.probe.resolve()), "--surface", "compare", "--output", str(out),
        "--first", str(root / a / (surface + ".png")), "--second", str(root / b / (surface + ".png"))]
    if crop:
        command += ["--region", ",".join(map(str, crop))]
    subprocess.run(command, env=env, check=True, capture_output=True, timeout=10)
    pixel = json.loads((out / "metrics.json").read_text(encoding="utf-8"))
    am, bm = runs[a], runs[b]
    return {"baseline":a, "candidate":b, "label":label, "pixels":pixel,
        "geometryEqual": am.get("caret", am.get("boxes")) == bm.get("caret", bm.get("boxes")),
        "displayLinesEqual": am.get("displayLines") == bm.get("displayLines")}
comparisons = [compare(a,b,label) for a,b,label in pairs]
for c in comparisons:
    if "mask-" in c["candidate"] or "preview-" in c["candidate"]:
        assert c["geometryEqual"] and c["displayLinesEqual"], c
assert runs["editor-mask-cached"]["imageHash"] == runs["editor-mask-uncached"]["imageHash"]
asset_comparisons = []
for a,b in [("preview-native", "preview-gamma-high"), ("preview-native", "preview-stroke"),
            ("preview-native-dark", "preview-gamma-dark"), ("preview-native-150", "preview-gamma-150")]:
    m = runs[a]
    box = next(item for item in m["boxes"] if item["id"] == "image")
    dpr = m["dpr"]
    # Interior of the neighboring image and SVG rectangles, excluding AA edges.
    crop = [math.ceil((box["x"] + 1) * dpr), math.ceil((box["y"] + 1) * dpr),
            math.floor(131 * dpr), math.floor(22 * dpr)]
    c = compare(a,b,"이미지·SVG 단색 영역",crop)
    assert c["pixels"]["changedPixels"] == 0, c
    asset_comparisons.append(c)
grayscale = compare("editor-native", "editor-mask-identity", "ASCII grayscale check", [0,0,800,16])
summary = {"grayscale":grayscale, "cases":len(records), "editorCases":sum(r["surface"] == "editor" for r in records),
    "comparisons":comparisons, "assetComparisons":asset_comparisons}
(root / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
items = []
for c in comparisons:
    a,b = c["baseline"], c["candidate"]
    surface = "editor" if a.startswith("editor-") else "preview"
    def data(name):
        return "data:image/png;base64," + base64.b64encode((root / name / (surface + ".png")).read_bytes()).decode("ascii")
    items.append({"label":c["label"], "base":data(a), "candidate":data(b),
        "metrics":{k:v for k,v in c.items() if k != "pixels"},
        "changedPixels":c["pixels"]["changedPixels"],
        "baselinePaintMs":runs[a].get("paintMedianMs"), "candidatePaintMs":runs[b].get("paintMedianMs")})
payload = json.dumps(items, ensure_ascii=False).replace("<", chr(92) + "u003c")
page = """<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>글꼴 렌더링 실험 비교</title><style>
*{box-sizing:border-box}body{margin:0;background:#f4f5f8;color:#182030;font:15px/1.6 'Malgun Gothic',sans-serif}
main{max-width:1500px;margin:24px auto;padding:0 24px}h1{font-size:25px;margin:0 0 10px}p{margin:8px 0}
.controls{display:flex;gap:16px;flex-wrap:wrap;align-items:center;background:white;padding:14px;margin:18px 0;border:1px solid #ddd;border-radius:8px}
select,input{font:inherit}select{min-width:350px;padding:6px}label{display:flex;gap:8px;align-items:center}
#viewport{overflow:auto;background:#dfe2e8;border:1px solid #bbc2ce;max-height:650px;padding:12px}
#stage{position:relative;display:inline-block;vertical-align:top;line-height:0}#stage img{display:block;max-width:none}
#candidate{position:absolute;top:0;left:0;clip-path:inset(0 50% 0 0)}
#boundary{position:absolute;top:0;bottom:0;left:50%;border-left:2px solid #ec4b39;pointer-events:none}
.legend{display:flex;justify-content:space-between;max-width:900px;font-weight:bold}.status{padding:14px;background:white;border:1px solid #ddd;border-radius:8px;margin-top:16px}
small{color:#535e70}button{font:inherit;padding:6px 12px;background:white;border:1px solid #aab2be;border-radius:5px;cursor:pointer}
</style><main><h1>I — 글꼴 렌더링 실험 비교</h1>
<p>실제 Scintilla Qt 포트와 Qt WebEngine 캡처. 100%·150% 배율, 밝은 테마·어두운 테마를 측정하였다.</p>
<small>MacType DLL 직접 주입은 미검증이다. 편집기 농도 보정과 Chromium 감마 보정은 서로 다른 계산 경로이다.</small>
<div class="controls"><label>실험 <select id="choice"></select></label><label>보정 영역 <input id="reveal" type="range" min="0" max="100" value="50"></label><button id="zoom">2배 확대</button></div>
<div class="legend"><span>← 보정 / 대체 경로</span><span>기본 경로 →</span></div>
<div id="viewport"><div id="stage"><img id="base" alt="기본 경로 캡처"><img id="candidate" alt="실험 경로 캡처"><div id="boundary"></div></div></div>
<div id="status" class="status"></div><p><small>픽셀 차이는 품질 향상의 점수가 아니다. 읽기 편한 정도는 원본 배율에서 비교한다. 성능은 960×360 viewport의 반복 grab 중앙값이며 실제 앱 전체 프레임 시간과 구분한다.</small></p>
<script id="data" type="application/json">__PAYLOAD__</script><script>
const entries=JSON.parse(document.getElementById('data').textContent), choice=document.getElementById('choice'),
base=document.getElementById('base'),candidate=document.getElementById('candidate'),reveal=document.getElementById('reveal'),stage=document.getElementById('stage');
let zoomed=false;
entries.forEach((e,i)=>choice.add(new Option(e.label,i)));choice.value=2;
function updateClip(){candidate.style.clipPath='inset(0 '+(100-reveal.value)+'% 0 0)';document.getElementById('boundary').style.left=reveal.value+'%';}
function updateZoom(){for(const img of [base,candidate]) img.style.width=img.naturalWidth*(zoomed?2:1)/window.devicePixelRatio+'px';}
function load(){const e=entries[choice.value];base.onload=updateZoom;candidate.onload=updateZoom;base.src=e.base;candidate.src=e.candidate;
const geometry=e.metrics.geometryEqual?'동일':'차이 있음';
let detail='변경 픽셀 '+e.changedPixels.toLocaleString()+' · 캐럿 / DOM 위치 '+geometry;
if(e.baselinePaintMs!=null)detail+=' · 반복 그리기 '+e.baselinePaintMs.toFixed(2)+' → '+e.candidatePaintMs.toFixed(2)+' ms';
document.getElementById('status').textContent=detail;updateClip();}
choice.addEventListener('change',load);reveal.addEventListener('input',updateClip);
document.getElementById('zoom').addEventListener('click',()=>{zoomed=!zoomed;document.getElementById('zoom').textContent=zoomed?'원본 픽셀':'2배 확대';updateZoom();});window.addEventListener('resize',updateZoom);load();
</script></main></html>""".replace("__PAYLOAD__",payload)
(root / "comparison.html").write_text(page.replace("\n", "\r\n"), encoding="utf-8")
print(json.dumps({"cases":len(records),"comparisons":len(comparisons),"assets":len(asset_comparisons),"status":"passed"}))

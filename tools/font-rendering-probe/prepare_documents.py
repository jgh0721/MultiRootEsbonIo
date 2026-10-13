"""Build opt-in integration fixtures with real Sphinx themes and pinned renderers.

Requires the application's Sphinx environment. Downloads only into the supplied
output directory; third-party packages and generated HTML are not committed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def fetch(url, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            destination.write_bytes(response.read())
    return {"url": url, "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}


def prepare(output):
    root = Path(__file__).resolve().parents[2]
    assets = output / "source" / "assets"
    manifest = []
    jobs = [
        ("https://cdn.jsdelivr.net/npm/katex@0.18.4/dist/katex.min.js", assets / "katex/katex.min.js"),
        ("https://cdn.jsdelivr.net/npm/katex@0.18.4/dist/katex.min.css", assets / "katex/katex.min.css"),
        ("https://cdn.jsdelivr.net/npm/mermaid@11.12.1/dist/mermaid.min.js", assets / "mermaid.min.js"),
        ("https://registry.npmjs.org/mathjax/-/mathjax-3.2.2.tgz", output / "downloads/mathjax-3.2.2.tgz"),
    ]
    with ThreadPoolExecutor(max_workers=4) as pool:
        manifest.extend(pool.map(lambda job: fetch(*job), jobs))
    css = (assets / "katex/katex.min.css").read_text(encoding="utf-8")
    fonts = sorted(set(re.findall(r"url\((fonts/[^)]+\.woff2)\)", css)))
    assert fonts, "No KaTeX web fonts found"
    with ThreadPoolExecutor(max_workers=6) as pool:
        manifest.extend(pool.map(lambda name: fetch(
            "https://cdn.jsdelivr.net/npm/katex@0.18.4/dist/" + name, assets / "katex" / name), fonts))
    # Keep extraction within the fixture; never extract symlinks or arbitrary paths.
    with tarfile.open(fileobj=io.BytesIO((output / "downloads/mathjax-3.2.2.tgz").read_bytes())) as archive:
        for member in archive:
            if not member.isfile() or not member.name.startswith("package/es5/"):
                continue
            relative = PurePosixPath(member.name).relative_to("package/es5")
            if ".." in relative.parts:
                raise ValueError("Unexpected archive path")
            path = assets / "mathjax" / Path(*relative.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(member).read())
    shutil.copyfile(root / "tools/js/vendor/markdown-it-15.0.0.umd.min.js", assets / "markdown-it.js")
    (assets / "ready.js").write_text(r"""
window.addEventListener('load', async () => {
    try {
        if (document.getElementById('markdown-source')) {
            document.getElementById('markdown-body').innerHTML = markdownit({html:true}).render(
                document.getElementById('markdown-source').textContent);
        }
        katex.render(String.raw`\frac{a^2+b^2}{\sqrt{x}}`, document.getElementById('katex-sample'));
        mermaid.initialize({startOnLoad:false, theme:'default', securityLevel:'strict'});
        await mermaid.run({querySelector:'.mermaid'});
        await MathJax.startup.promise;
        await MathJax.typesetPromise();
        await document.fonts.ready;
        document.documentElement.dataset.renderReady = 'true';
    } catch (error) {
        document.documentElement.dataset.renderError = String(error);
    }
});
""", encoding="utf-8")
    content = """<p id="sample">Preview text 한글 <strong>bold</strong> <em>italic</em> and <code>code</code>.</p>
<p id="katex-sample"></p>
<div class="math notranslate nohighlight">\\[E=mc^2 + \\frac{a}{b}\\]</div>
<pre class="mermaid">flowchart LR
    A[Start 한글] --> B[Result]</pre>
<p><span class="fa fa-home" id="font-icon" aria-hidden="true"></span> Icon</p>
<svg id="vector" width="50" height="30"><rect width="50" height="30" fill="orange"/></svg>
"""
    source = output / "source"
    (source / "index.rst").write_text("Rendering integration\n=====================\n\n"
        ".. math::\n\n   x^2 + y^2 = z^2\n\n.. raw:: html\n\n" + "\n".join("   " + line for line in content.splitlines()) + "\n",
        encoding="utf-8")
    (source / "conf.py").write_text("""project = 'Rendering integration'
extensions = ['sphinx.ext.mathjax']
html_static_path = ['assets']
html_css_files = ['katex/katex.min.css']
html_js_files = ['katex/katex.min.js', 'mermaid.min.js', 'ready.js']
mathjax_path = 'mathjax/tex-chtml.js'
html_theme = 'alabaster'
html_theme_options = {}
""", encoding="utf-8")
    for theme in ("alabaster", "sphinx_rtd_theme"):
        subprocess.run([sys.executable, "-m", "sphinx", "-q", "-E", "-b", "html",
                        "-D", "html_theme=" + theme, str(source), str(output / theme)], check=True)
    markdown = output / "markdown"
    markdown.mkdir(exist_ok=True)
    shutil.copytree(assets, markdown / "_static", dirs_exist_ok=True)
    (markdown / "index.html").write_text("""<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="_static/katex/katex.min.css">
<style>body{font:16px Arial;margin:20px}pre{background:#eee;padding:12px}code{font-family:Consolas}</style>
<script src="_static/markdown-it.js"></script><script src="_static/katex/katex.min.js"></script>
<script src="_static/mermaid.min.js"></script><script src="_static/mathjax/tex-chtml.js"></script>
<script src="_static/ready.js"></script></head><body>
<script type="text/plain" id="markdown-source"># Markdown rendering

""" + content + "\n</script><article id=\"markdown-body\"></article></body></html>", encoding="utf-8")
    (output / "assets.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("Fixtures ready:", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    prepare(parser.parse_args().output.resolve())

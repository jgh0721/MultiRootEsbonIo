import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mrst_preview_build", ROOT / "tools/py/mrr_sphinx_preview_build.py")
preview = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = preview
spec.loader.exec_module(preview)


class PreviewInputsTests(unittest.TestCase):
    def test_real_sphinx_template_override_and_fingerprint(self):
        from sphinx.application import Sphinx
        from io import StringIO
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            source = base / "source"
            source.mkdir()
            templates = source / "_templates"
            templates.mkdir()
            (source / "conf.py").write_text("templates_path=['_templates']\nhtml_theme='basic'\n", encoding="utf-8")
            (source / "index.rst").write_text("Title\n=====\n\nBody\n", encoding="utf-8")
            layout = templates / "layout.html"
            layout.write_text("{% extends '!layout.html' %}{% block footer %}MRST_OLD{% endblock %}", encoding="utf-8")
            app = Sphinx(str(source), str(source), str(base / "html"), str(base / "doctrees"),
                         "html", status=StringIO(), warning=StringIO())
            app.build()
            inputs = base / "inputs.json"
            preview._save_inputs(inputs, app, source, source)
            saved = json.loads(inputs.read_text(encoding="utf-8"))
            self.assertEqual(saved["schema"], 2)
            self.assertTrue(saved["templatesComplete"])
            self.assertIn(str(layout.resolve()), {entry["p"] for entry in saved["files"]})
            self.assertIn(str(templates.resolve()), {entry["p"] for entry in saved["templateDirs"]})
            self.assertGreater(len(saved["templateDirs"]), 1)  # builtin inherited theme
            self.assertIn("MRST_OLD", (base / "html/index.html").read_text(encoding="utf-8"))
            old = layout.stat().st_mtime_ns
            layout.write_text("{% extends '!layout.html' %}{% block footer %}MRST_CHANGED{% endblock %}", encoding="utf-8")
            os.utime(layout, ns=(old + 2_000_000_000, old + 2_000_000_000))
            app.build()
            self.assertIn("MRST_CHANGED", (base / "html/index.html").read_text(encoding="utf-8"))

    def test_missing_root_is_tracked_and_nested_addition_changes_list(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder)
            root = source / "_templates"
            app = SimpleNamespace(config=SimpleNamespace(templates_path=["_templates"]),
                                  builder=SimpleNamespace(templates=SimpleNamespace(pathchain=[])))
            before = preview._template_directories(app, source)
            self.assertEqual(before[str(root.resolve())], [])
            (root / "nested").mkdir(parents=True)
            (root / "nested/layout.html").write_text("override", encoding="utf-8")
            after = preview._template_directories(app, source)
            self.assertEqual(after[str(root.resolve())], ["nested/layout.html"])


if __name__ == "__main__":
    unittest.main()

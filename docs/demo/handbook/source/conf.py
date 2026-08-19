# Configuration file for the Sphinx documentation builder.
#
# Demo project 1 of 5 in this workspace. Standard layout: conf.py lives under
# "source/", so the editor resolves this project's root to handbook/source.

project = "Orca Engine Handbook"
copyright = "2026, MultiRoot reST Editor demo"
author = "MultiRoot reST Editor"
version = "3.2"
release = "3.2.0"

extensions = [
    "sphinx.ext.doctest",
    "sphinx_design",
    "sphinx_copybutton",
    "sphinxcontrib.mermaid",
]

templates_path = ["_templates"]
exclude_patterns = ["_build", "_snippets/*"]
language = "en"

numfig = True

rst_prolog = """
.. |product| replace:: Orca Engine
.. |br| raw:: html

   <br/>
"""

# Furo follows prefers-color-scheme, so this project's preview turns dark when
# the surrounding desktop does. The other demo projects deliberately use themes
# that stay light - switching tabs makes the per-project mapping visible.
html_theme = "furo"
html_static_path = ["_static"]
html_title = "Orca Engine Handbook 3.2"

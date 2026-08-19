# 데모 프로젝트 2/5. 경로와 project 이름에 한글과 공백이 들어 있다.
# 편집기가 한글 경로를 그대로 다루는지 보여 주는 자리다.

project = "오르카 엔진 사용자 안내서"
copyright = "2026, MultiRoot reST Editor 데모"
author = "MultiRoot reST Editor"
version = "3.2"
release = "3.2.0"

extensions = []

templates_path = ["_templates"]
exclude_patterns = ["_build", "_snippets/*"]
language = "ko"

numfig = True
numfig_format = {"figure": "그림 %s", "table": "표 %s", "code-block": "코드 %s"}

rst_prolog = """
.. |제품| replace:: 오르카 엔진
"""

html_theme = "sphinx_rtd_theme"
html_static_path = []

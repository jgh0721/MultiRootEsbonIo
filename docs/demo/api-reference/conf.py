# 데모 프로젝트 3/5. source/ 하위 디렉터리 없이 프로젝트 루트에 conf.py 가 바로 있다.
# 1, 2 번과 배치가 다른데도 같은 방식으로 인식되는 것을 보이기 위한 구성이다.

project = "Orca Engine API Reference"
copyright = "2026, MultiRoot reST Editor demo"
author = "MultiRoot reST Editor"
version = "3.2"
release = "3.2.0"

extensions = []

exclude_patterns = ["_build"]
language = "en"

html_theme = "pydata_sphinx_theme"
html_theme_options = {
    "show_prev_next": False,
    "navbar_end": ["theme-switcher"],
}

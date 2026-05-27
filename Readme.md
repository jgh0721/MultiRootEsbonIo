
# MultiRoot reStructuredText Editor, By esbonio


해당 프로젝트는 esbonio 가 하나의 폴더에 다수의 문서 프로젝트가 포함되어 있을 때 이를 지원하지 않는 것을 극복하기 위한 프로젝트입니다. 

예) 

    Root
        DocA
            conf.py
            index.rst
        DocB
            conf.py
            index.rst
        DocC
            source
                conf.py
                index.rst
        examples.rst

위와 같이 문서 프로젝트가 있을 때, 어떤 파일을 열어서 작업하더라도 정확하게 프로젝트를 매핑하여 구문 강조 등을 제공합니다. 

개발 환경

MSVC 2022, Qt 6.11.1, Windows X64

## 기능

* Scintilla 와 Lexilla 
* 자체 내장된 uv 를 사용하여 python 환경 구축
* 개요 
  * esbonio 에서 제공하는 정보 
* 진단 정보
  * esbonio 와 sphinx-build 를 통해 정보를 취합하여 진단 정보 표시
* 자동완성
  * esbonio lsp 로부터 정보를 취합하여 표시
* 스크롤 동기화
  * 코드 편집기 <-> 프리뷰 양방향 지원
* 가상 프로젝트
  * conf.py 파일이 없는 단독 .rst 파일을 지원
 
※ 해당 프로젝트는 AI 를 사용하여 작성되었습니다. 

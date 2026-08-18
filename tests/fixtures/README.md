# 테스트 픽스처

## TestWorkspace

멀티루트 동작과 스크롤 동기화를 손으로 확인할 때 쓰는 워크스페이스입니다.

```
TestWorkspace/
  DocA/conf.py, index.rst, _images/     root_doc = 'index'
  DocB/conf.py, source/index.rst        master_doc = 'source/index'  (source 경로 추론 확인용)
  DocC/source/conf.py, source/index.rst 중첩 프로젝트 (projectId = DocC.source)
  orphan.rst                            conf.py 없는 단독 파일 (가상 프로젝트 확인용)
```

`DocC/source/conf.py` 에는 레거시 `html_style = ''` 가 들어 있습니다.
Sphinx 8 에서 `_static` checksum 오류를 내는 설정이라, override 가 동작하는지
확인하는 용도입니다.

### 실행

```powershell
# 워크스페이스만 열기
"MultiRoot-reST Editor.exe" <경로>\TestWorkspace

# 세 프로젝트의 파일을 동시에 열기 (LSP 풀 확인)
"MultiRoot-reST Editor.exe" <경로>\TestWorkspace `
    <경로>\TestWorkspace\DocA\index.rst `
    <경로>\TestWorkspace\DocB\source\index.rst `
    <경로>\TestWorkspace\DocC\source\index.rst
```

이미 열린 파일을 다시 인자로 주면 그 탭으로 전환되므로, 탭 전환(= LSP 재기동)
시나리오도 명령줄만으로 만들 수 있습니다.

### 진단용 환경변수

| 변수 | 내용 |
|---|---|
| `MRST_LOG_FILE` | 로그 창 내용을 파일로 미러링 |
| `MRST_LSP_TRACE` | JSON-RPC 프레임 원문 기록 |
| `MV_TEXT_LEXER_TRACE_FILE` | 어떤 lexer 가 적용됐는지 기록 |

## DocA/index.rst 가 담고 있는 것

스크롤 동기화는 "소스 줄 간격"과 "렌더 픽셀 간격"이 비례하지 않는 데서 깨집니다.
그 비율이 최대한 넓게 흩어지도록 구성했습니다.

| 요소 | 소스 줄 대비 렌더 높이 |
|---|---|
| 긴 문단 | 1줄이 여러 행으로 접힘 (px/line 큼) |
| 큰 이미지 (480x300) | 2줄이 300px 이상 |
| 작은 이미지 (200x80) | 2줄이 80px |
| 그리드 테이블 | 경계선 때문에 소스 줄이 많고 렌더는 조밀 (px/line 작음) |
| 간단 테이블 | 위와 비슷하지만 경계선이 적음 |
| list-table (8컬럼) | 셀 하나가 소스 1줄인데 한 행의 셀들이 모두 같은 높이 |
| list-table (30행) | 소스는 60줄 이상, 렌더는 규칙적 |
| code-block | 소스와 렌더가 거의 1:1 |

실측하면 구간별 px/line 이 **1.0 ~ 114.3 (약 114배)** 까지 벌어집니다.
줄 수에 비례해 스크롤을 맞추는 구현은 이 문서에서 반드시 어긋납니다.

### 스크롤 매핑 직접 확인하기

프리뷰 페이지에 `window.__mrrTestHooks` 가 노출되어 있습니다.
(DevTools 콘솔 또는 `QWebEnginePage::runJavaScript`)

```js
var h = window.__mrrTestHooks;
var t = h.anchorTable();            // 보간 기준점 목록

// 구간별 소스줄 대비 픽셀 비율
t.slice(0, -1).map(function (p, i) {
  return 'L' + p.line + '->' + t[i+1].line
       + ' px/line=' + ((t[i+1].y - p.y) / (t[i+1].line - p.line)).toFixed(1);
});

// 왕복 정확도: line -> Y -> line 이 제자리로 돌아와야 한다
t.filter(function (p) { return p.src === 0; })
 .map(function (p) { return h.lineForDocumentY(h.documentYForLine(0, p.line)).line - p.line; })
 .reduce(function (worst, e) { return Math.max(worst, Math.abs(e)); }, 0);   // 0 이어야 함
```

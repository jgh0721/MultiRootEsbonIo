include_guard(GLOBAL)

# windeployqt 인자는 여기 한 곳에서만 정한다.
#
# 개발 빌드의 POST_BUILD 와 릴리스 패키징이 서로 다른 인자로 배포하면
# "개발에서는 되는데 배포본만 죽는" 재현하기 어려운 버그가 된다.
#
#   --no-translations        : Qt 자체 .qm 은 쓰지 않는다
#                              (WebEngine locales 는 이 옵션과 무관하게 복사된다)
#   --no-compiler-runtime    : MSVC 재배포 DLL 은 넣지 않는다
#   --no-system-d3d-compiler : 시스템 d3dcompiler 를 신뢰한다
set(MRST_WINDEPLOYQT_ARGS
        --no-translations
        --no-compiler-runtime
        --no-system-d3d-compiler
)

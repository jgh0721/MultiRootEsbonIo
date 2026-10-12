#pragma once

#include <QString>

namespace mrst {
/// 입력 파일과 템플릿 목록을 비교한다. 구 버전·알 수 없는 지문은 다시 빌드한다.
bool previewInputsChanged(const QString& inputsFile);
} // namespace mrst

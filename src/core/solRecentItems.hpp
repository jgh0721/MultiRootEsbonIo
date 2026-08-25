#pragma once

#include <QString>
#include <QStringList>

namespace mrst {

/// 최근 목록 하나가 들고 있는 항목 수의 상한.
///
/// 파일과 워크스페이스가 **같은 메뉴**에 위아래로 놓이므로, 둘을 합쳐도 메뉴가
/// 화면 밖으로 넘치지 않는 값이어야 한다.
inline constexpr int kMaxRecentEntries = 10;

/// `entry` 를 맨 앞에 놓은 새 목록을 돌려준다.
///
/// 이미 있으면 옮기기만 한다 — 중복이 생기지 않는다. 비교는 파일 시스템의 규칙을
/// 따른다. Windows 에서 `D:\Doc` 과 `d:\doc` 은 같은 폴더인데 그것을 둘로 세면
/// 사용자는 같은 항목이 두 번 적힌 메뉴를 본다.
///
/// 표기도 함께 통일한다(`QDir::toNativeSeparators` + `cleanPath`). 워크스페이스
/// 경로는 파일 대화상자에서 `D:/doc` 으로, 드래그 앤 드롭에서 `D:\doc` 으로 들어와
/// 정규화하지 않으면 같은 폴더가 두 항목으로 남는다.
///
/// 상한을 넘으면 뒤에서 자른다. `entry` 가 비었으면 목록을 그대로 돌려준다.
[[nodiscard]] QStringList prependRecentEntry( const QStringList& entries, const QString& entry,
                                              int maximum = kMaxRecentEntries );

/// 목록에서 `entry` 를 뺀다. 없으면 그대로 돌려준다. 비교 규칙은 위와 같다.
///
/// 지워진 파일이나 사라진 폴더를 메뉴에서 골랐을 때 쓴다. 그대로 두면 사용자가
/// 같은 실패를 반복할 때까지 목록의 자리를 차지한다.
[[nodiscard]] QStringList removeRecentEntry( const QStringList& entries, const QString& entry );

}  // namespace mrst

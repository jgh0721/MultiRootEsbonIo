#pragma once

#include <QIcon>
#include <QPalette>

namespace mrst::panelicons {

/// 패널 도구 줄에 쓰는 단색 아이콘.
///
/// 그려서 만든다. 저장소에 아이콘 자산이 하나도 없고(resources/ 에는 앱
/// 아이콘과 테마 JSON 뿐이다), Qlementine 도 메시지 상자용 넷만 들고 있다.
/// SVG 를 들이면 라이트/다크 두 벌을 관리하거나 실행 시점에 색을 갈아 끼워야
/// 하는데, 어느 쪽이든 이 다섯 개를 위해 치를 비용은 아니다.
///
/// **테마는 인자로 받는 팔레트가 정한다.** 색을 상수로 두지 않았으므로 라이트와
/// 다크가 저절로 갈린다 — 어두운 테마에서 검은 아이콘이 남는 부류의 버그가
/// 생길 자리가 없다. 사용 불가 상태도 같은 팔레트의 Disabled 그룹에서 온다.
///
/// QIcon 은 만들어진 픽스맵을 들고 있을 뿐 팔레트를 따라가지 않는다. 테마가
/// 바뀌면 호출 측이 다시 만들어야 한다 (MainWindow::applyExplorerIcons).
[[nodiscard]] QIcon newFile( const QPalette& palette );
[[nodiscard]] QIcon newFolder( const QPalette& palette );
[[nodiscard]] QIcon rename( const QPalette& palette );
[[nodiscard]] QIcon remove( const QPalette& palette );
/// 필터 입력칸 앞머리에 붙는 돋보기.
[[nodiscard]] QIcon filter( const QPalette& palette );

}  // namespace mrst::panelicons

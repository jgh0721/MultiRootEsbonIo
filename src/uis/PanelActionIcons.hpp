#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QSize>

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
/// 탐색기의 "모든 파일 표시" 토글. 켜면 확장자 필터를 걷어낸다.
///
/// 눈이다. 같은 줄의 다른 아이콘(문서·폴더·연필·휴지통·돋보기)과 형태가 겹치지
/// 않아야 16px 에서 갈리는데, 가로로 누운 아몬드는 그 넷 어느 것과도 닮지 않았다.
/// 뜻도 곧바로 읽힌다 — 이 단추가 하는 일이 "감춰 둔 것을 보이기" 다.
[[nodiscard]] QIcon showAllFiles( const QPalette& palette );

/// 설정 → 테마의 색상 칸에 놓는 색 견본.
///
/// **아이콘이어야 한다.** `QTableWidgetItem::setBackground()` 로는 안 된다 —
/// Qlementine 이 `PE_PanelItemViewItem` 에서 셀 배경을 직접 칠하면서
/// `QStyleOptionViewItem::backgroundBrush` 를 읽지 않고, 색을 정하는
/// `listItemBackgroundColor()` 는 모델 인덱스를 아예 무시한다(`Q_UNUSED(index)`).
/// 그래서 브러시로 색을 실어 보내면 모든 칸이 같은 색으로 나온다. 아이콘은
/// `QStyleOptionViewItem::icon` 을 타고 가므로 그 경로를 지나지 않는다.
///
/// 테두리는 장식이 아니다 — 없으면 배경과 같은 색인 견본이 빈 칸처럼 보인다.
/// 알파가 있는 색은 체커보드 위에 올린다. 그러지 않으면 반투명한 색과 불투명한
/// 색을 화면에서 구분할 수 없다.
///
/// 격자와 테두리 색은 팔레트에서 온다. `panelicons` 의 다른 함수들과 같은
/// 이유다 — 라이트에서 흰 격자, 다크에서 검은 격자가 되어야 한다.
[[nodiscard]] QIcon colorSwatch( const QColor& color, const QSize& size, const QPalette& palette );

}  // namespace mrst::panelicons

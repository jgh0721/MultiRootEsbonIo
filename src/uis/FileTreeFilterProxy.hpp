#pragma once

#include <QCollator>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QString>

namespace mrst {

/// Windows 드라이브 경로가 원격 매핑이고 현재 세션이 끊겨 있는가.
/// 로컬 경로, 연결된 원격 드라이브, 상태를 확인할 수 없는 공급자는 false 이다.
[[nodiscard]] bool isDisconnectedRemoteDrivePath( const QString& path );

/// 탐색기 트리의 필터 · 정렬을 맡는 프록시.
///
/// QFileSystemModel 위에서는 그 모델의 `isDir()` 로 디렉터리를 판정한다.
/// 다른 모델은 `hasChildren()` 으로 되돌아가므로 규칙 전체를 파일 시스템 없이도
/// 검증할 수 있다. Windows 의 연결 끊긴 원격 드라이브 루트는 뷰에 내보내지 않는다.
///
/// **QFileSystemModel 은 게으르다.** 아직 펼치지 않은 디렉터리는 자식이
/// 모델에 들어와 있지 않아 필터가 볼 수 없다. 그래서 필터가 켜져 있는 동안에는
/// 호출 측이 트리를 펼쳐 주어야 하고(MainWindow::refreshExplorerFilter),
/// 디렉터리가 뒤늦게 읽히면 그때 다시 펼친다. VS Code 의 탐색기 필터도 같은
/// 성질을 갖는다 — 읽어 들인 범위 안에서 거른다.
class FileTreeFilterProxy final : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit FileTreeFilterProxy( QObject* parent = nullptr );

    /// 필터 문구. 비면 전부 통과한다.
    ///
    /// `*` 나 `?` 가 들어 있으면 와일드카드로, 아니면 부분 일치로 본다.
    /// 파일 트리에서 `*.rst` 는 사람들이 실제로 치는 형태다.
    void                                setFilterText( const QString& text );
    [[nodiscard]] QString               filterText() const { return filterText_; }
    [[nodiscard]] bool                  isFiltering() const { return !filterText_.isEmpty(); }

    /// 조상 검사를 멈출 자리 (트리 뷰의 루트에 해당하는 **원본** 인덱스).
    ///
    /// 이름이 맞은 디렉터리 아래는 통째로 보여 주는데, 그 규칙에 뿌리를 두지
    /// 않으면 워크스페이스 폴더 이름이 우연히 맞는 순간 필터가 통째로 무력해진다
    /// (`D:\docs` 에서 "doc" 을 치는 것이 정확히 그 경우다).
    void                                setRootSourceIndex( const QModelIndex& index );

protected:
    [[nodiscard]] bool                  filterAcceptsRow( int sourceRow,
                                                          const QModelIndex& sourceParent ) const override;
    [[nodiscard]] bool                  lessThan( const QModelIndex& left,
                                                  const QModelIndex& right ) const override;

private:
    [[nodiscard]] bool                  matches( const QModelIndex& sourceIndex ) const;
    /// 이 인덱스가 뷰의 뿌리이거나 그 조상인가. 그런 행은 걸러 내면 안 된다 —
    /// 이유는 filterAcceptsRow 안에 적어 두었다.
    [[nodiscard]] bool                  isRootOrAncestor( const QModelIndex& sourceIndex ) const;
    [[nodiscard]] bool                  isDirectory( const QModelIndex& sourceIndex ) const;
    /// Windows 파일 모델의 최상위 드라이브 중, 매핑은 남아 있지만 세션이 끊겼는가.
    [[nodiscard]] bool                  isDisconnectedRemoteDrive(
        const QModelIndex& sourceIndex ) const;

    QString                             filterText_;
    /// 와일드카드일 때만 채워진다.
    QRegularExpression                  wildcard_;
    QPersistentModelIndex               rootSourceIndex_;
    /// "file2" 가 "file10" 앞에 오게 한다. 만드는 비용이 있어 하나만 둔다.
    QCollator                           collator_;
};

}  // namespace mrst

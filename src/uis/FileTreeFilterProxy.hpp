#pragma once

#include <QCollator>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QString>

namespace mrst {

/// 탐색기 트리의 필터 · 정렬을 맡는 프록시.
///
/// QFileSystemModel 위에 얹는 것을 전제로 만들었지만 그것에 매이지는 않는다 —
/// 디렉터리 판정만 QFileSystemModel 일 때 쓰고, 아니면 이름만으로 비교한다.
/// 그래야 파일 시스템 없이도 검증할 수 있다.
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
    [[nodiscard]] bool                  isDirectory( const QModelIndex& sourceIndex ) const;

    QString                             filterText_;
    /// 와일드카드일 때만 채워진다.
    QRegularExpression                  wildcard_;
    QPersistentModelIndex               rootSourceIndex_;
    /// "file2" 가 "file10" 앞에 오게 한다. 만드는 비용이 있어 하나만 둔다.
    QCollator                           collator_;
};

}  // namespace mrst

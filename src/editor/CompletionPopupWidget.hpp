#pragma once

#include <QFrame>
#include <QList>
#include <QString>
#include <QVector>

class QKeyEvent;
class QListWidget;

namespace mrst {

struct CompletionDisplayItem
{
    QString                             label;
    QString                             insertText;
    QString                             detail;
    int                                 kind = 0;        ///< LSP CompletionItemKind
    QString                             filterText;      ///< 비어 있으면 label 로 거른다
};

/// 부분 일치(subsequence) 검사와 점수 매기기.
///
/// 연속 일치·단어 경계·접두 일치에 가산점을 주고 건너뛴 글자에 감점한다.
/// matchedPositions 는 강조 표시용 candidate 기준 문자 인덱스.
[[nodiscard]] bool fuzzyMatchCompletion( const QString& pattern, const QString& candidate,
                                         int* score = nullptr,
                                         QVector< int >* matchedPositions = nullptr );

/// 편집기 위에 뜨는 자동완성 목록.
///
/// **포커스를 절대 가져가지 않는다.** 캐럿이 편집기에 남아 있어야 계속 타이핑할
/// 수 있기 때문이다. 방향키·Enter·Esc 는 편집기 쪽 이벤트 필터가 가로채
/// handleKeyPress() 로 넘긴다.
class CompletionPopupWidget final : public QFrame
{
    Q_OBJECT

public:
    explicit CompletionPopupWidget( QWidget* parent = nullptr );

    void                                setItems( const QList< CompletionDisplayItem >& items );
    /// 이미 입력한 접두로 목록을 좁힌다. 남는 것이 없으면 스스로 숨는다.
    void                                updateFilter( const QString& prefix );
    /// 화면 밖으로 나가지 않도록 보정한 뒤 표시한다.
    void                                showAt( const QPoint& globalTopLeft );

    [[nodiscard]] bool                  isActive() const;
    [[nodiscard]] int                   visibleCount() const;
    /// 지금 강조된 항목. 목록이 비었으면 label 이 빈 항목.
    [[nodiscard]] CompletionDisplayItem currentItem() const;

    void                                selectNext();
    void                                selectPrevious();
    /// 선택된 항목을 확정한다. 확정할 것이 있었으면 true.
    bool                                acceptCurrent();
    /// 편집기가 받은 키를 팝업이 대신 처리한다. 처리했으면 true.
    bool                                handleKeyPress( QKeyEvent* event );

signals:
    void                                itemSelected( const QString& insertText );
    /// 강조된 항목이 바뀌었다. 상세 패널을 갱신하는 데 쓴다.
    void                                currentItemChanged( const CompletionDisplayItem& item );

private:
    void                                rebuild();
    void                                resizeToRows();
    [[nodiscard]] QString               currentInsertText() const;
    bool                                selectByInsertText( const QString& insertText );
    void                                selectFirst();

    QListWidget*                        list_ = nullptr;
    QList< CompletionDisplayItem >      allItems_;
    QString                             prefix_;
};

}  // namespace mrst

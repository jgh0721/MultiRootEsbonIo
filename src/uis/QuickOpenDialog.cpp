#include "stdafx.h"
#include "uis/QuickOpenDialog.hpp"

#include "core/solQuickOpenSearch.hpp"

#include <QAccessible>
#include <QAbstractItemView>
#include <QAbstractListModel>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <utility>

namespace mrst {
namespace {

constexpr int kPageSize = 150;
constexpr int kPrefetchThreshold = 20;
constexpr auto kRankingDebounce = std::chrono::milliseconds{ 90 };
constexpr auto kIncrementalRankingThrottle = std::chrono::milliseconds{ 500 };
constexpr auto kIncrementalRankingInputIdle = std::chrono::milliseconds{ 350 };
constexpr auto kFooterUpdateThrottle = std::chrono::milliseconds{ 100 };
constexpr int kDialogMinimumWidth = 480;
constexpr int kDialogMinimumHeight = 340;
constexpr int kDialogPreferredWidth = 760;
constexpr int kDialogPreferredHeight = 460;

QThreadPool& quickOpenRetirementPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount( 1 );
        pool.setExpiryTimeout( 30'000 );
        pool.setThreadPriority( QThread::LowestPriority );
        return true;
    }();
    Q_UNUSED( configured );
    return pool;
}

void retireQuickOpenPathChunks( QuickOpenPathChunks chunks )
{
    if( chunks.isEmpty() )
        return;
    quickOpenRetirementPool().start(
        [ chunks = std::move( chunks ) ]() mutable { chunks.clear(); } );
}

void retireQuickOpenMatches(
    std::shared_ptr< const QuickOpenRankedMatches > matches )
{
    if( matches == nullptr || matches->isEmpty() )
        return;
    quickOpenRetirementPool().start(
        [ matches = std::move( matches ) ]() mutable { matches.reset(); } );
}

QString pathKey( const QString& relativePath )
{
#ifdef Q_OS_WIN
    return relativePath.toCaseFolded();
#else
    return relativePath;
#endif
}

/// 네이티브 IME가 조합 중인 동안 Enter/Esc/방향키를 빠른 열기 명령으로
/// 가로채지 않게 preedit 수명을 기억한다. QLineEdit 자체의 입력기 처리는
/// 그대로 호출하므로 한글 후보 선택과 조합 확정은 플랫폼 동작을 따른다.
class QuickOpenSearchEdit final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

    [[nodiscard]] bool isComposing() const noexcept
    {
        return composing_;
    }

    [[nodiscard]] QString effectiveSearchText() const
    {
        QString effective = text();
        if( preeditText_.isEmpty() )
            return effective;

        const qsizetype insertion = (std::clamp)(
            static_cast<qsizetype>( preeditPosition_ ), qsizetype{ 0 },
            effective.size() );
        effective.insert( insertion, preeditText_ );
        return effective;
    }

protected:
    void inputMethodEvent( QInputMethodEvent* event ) override
    {
        const bool wasComposing = composing_;
        const int insertionBeforeEvent = selectionStart() >= 0
                                             ? selectionStart()
                                             : cursorPosition();
        QLineEdit::inputMethodEvent( event );

        preeditText_ = event != nullptr ? event->preeditString() : QString{};
        composing_ = !preeditText_.isEmpty();
        if( composing_ )
        {
            if( !wasComposing )
                preeditPosition_ = insertionBeforeEvent;
            else if( event != nullptr && !event->commitString().isEmpty() )
                preeditPosition_ = cursorPosition();
        }
        else
            preeditPosition_ = cursorPosition();
    }

    void keyPressEvent( QKeyEvent* event ) override
    {
        if( composing_ && event != nullptr
            && ( event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
                 || event->key() == Qt::Key_Escape ) )
        {
            QLineEdit::keyPressEvent( event );
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent( event );
    }

private:
    QString preeditText_;
    int     preeditPosition_ = 0;
    bool    composing_ = false;
};

/// 프레임 없는 대화상자에서도 헤더를 잡아 옮길 수 있게 한다. 운영체제의
/// system move를 써서 다중 모니터/DPI 경계와 스냅 동작을 직접 흉내 내지 않는다.
class QuickOpenHeader final : public QFrame
{
public:
    using QFrame::QFrame;

protected:
    void mousePressEvent( QMouseEvent* event ) override
    {
        if( event != nullptr && event->button() == Qt::LeftButton )
        {
            if( QWindow* handle = window()->windowHandle(); handle != nullptr
                && handle->startSystemMove() )
            {
                event->accept();
                return;
            }
        }
        QFrame::mousePressEvent( event );
    }
};

struct ElidedRun
{
    QString      text;
    QVector<int> matchedPositions;
};

/// QFontMetrics가 만든 가운데/오른쪽 말줄임표에 원문의 매치 위치를 다시
/// 대응시킨다. 말줄임표로 사라진 문자는 강조하지 않는다.
ElidedRun elideKeepingMatches( const QString& text, const QVector<int>& positions,
                               const QFontMetrics& metrics, const int width,
                               const Qt::TextElideMode mode )
{
    ElidedRun run = { text, positions };
    if( width <= 0 )
    {
        run.text.clear();
        run.matchedPositions.clear();
        return run;
    }
    if( metrics.horizontalAdvance( text ) <= width )
        return run;

    run.text = metrics.elidedText( text, mode, width );
    run.matchedPositions.clear();

    const int ellipsis = run.text.indexOf( QChar( 0x2026 ) );
    if( ellipsis < 0 )
    {
        for( const int position : positions )
        {
            if( position >= 0 && position < run.text.size() )
                run.matchedPositions.append( position );
        }
        return run;
    }

    const int prefixLength = ellipsis;
    const int suffixLength = run.text.size() - ellipsis - 1;
    const int suffixStart = text.size() - suffixLength;
    for( const int position : positions )
    {
        if( position >= 0 && position < prefixLength )
            run.matchedPositions.append( position );
        else if( suffixLength > 0 && position >= suffixStart && position < text.size() )
            run.matchedPositions.append( ellipsis + 1 + position - suffixStart );
    }
    return run;
}

QVector<QTextLayout::FormatRange> matchFormats( const QVector<int>& positions,
                                                const QColor& color,
                                                const bool strengthenWeight )
{
    QVector<int> sorted = positions;
    std::sort( sorted.begin(), sorted.end() );
    sorted.erase( std::unique( sorted.begin(), sorted.end() ), sorted.end() );

    QVector<QTextLayout::FormatRange> ranges;
    for( qsizetype i = 0; i < sorted.size(); )
    {
        if( sorted.at( i ) < 0 )
        {
            ++i;
            continue;
        }

        const int start = sorted.at( i );
        int length = 1;
        while( i + length < sorted.size() && sorted.at( i + length ) == start + length )
            ++length;

        QTextCharFormat format;
        format.setForeground( color );
        format.setFontUnderline( true );
        if( strengthenWeight )
            format.setFontWeight( QFont::DemiBold );
        ranges.append( QTextLayout::FormatRange{ start, length, format } );
        i += length;
    }
    return ranges;
}

void drawTextRun( QPainter* painter, const QRect& rect, const QString& text, const QFont& font,
                  const QColor& baseColor, const QColor& matchColor,
                  const QVector<int>& positions, const bool strengthenWeight,
                  const Qt::LayoutDirection direction, const Qt::Alignment alignment )
{
    if( painter == nullptr || rect.isEmpty() || text.isEmpty() )
        return;

    QTextLayout layout( text, font, painter->device() );
    QTextOption textOption;
    textOption.setWrapMode( QTextOption::NoWrap );
    textOption.setTextDirection( direction );
    textOption.setAlignment( alignment );
    layout.setTextOption( textOption );

    QVector<QTextLayout::FormatRange> formats = matchFormats( positions, matchColor,
                                                              strengthenWeight );
    QTextCharFormat baseFormat;
    baseFormat.setForeground( baseColor );
    formats.prepend( QTextLayout::FormatRange{ 0, static_cast< int >( text.size() ), baseFormat } );
    layout.setFormats( formats );

    layout.beginLayout();
    QTextLine line = layout.createLine();
    if( line.isValid() )
    {
        line.setLineWidth( rect.width() );
        line.setPosition( QPointF( 0.0, ( rect.height() - line.height() ) / 2.0 ) );
    }
    layout.endLayout();
    layout.draw( painter, rect.topLeft() );
}

}  // namespace

class QuickOpenListModel final : public QAbstractListModel
{
    Q_DECLARE_TR_FUNCTIONS( mrst::QuickOpenListModel )

public:
    using MatchList = QuickOpenRankedMatches;
    using MatchListPtr = std::shared_ptr< const MatchList >;

    enum DataRole
    {
        RelativePathRole = Qt::UserRole + 1,
        FileNameRole,
        DirectoryPathRole,
        FileNameMatchesRole,
        PathMatchesRole
    };

    explicit QuickOpenListModel( QObject* parent = nullptr )
        : QAbstractListModel( parent )
    {
    }

    [[nodiscard]] int rowCount( const QModelIndex& parent = {} ) const override
    {
        return parent.isValid() ? 0 : visibleCount_;
    }

    [[nodiscard]] QVariant data( const QModelIndex& index, const int role ) const override
    {
        if( !index.isValid() || index.row() < 0 || index.row() >= visibleCount_ )
            return {};

        const QuickOpenMatch& match = matches_->at( index.row() ).match;
        switch( role )
        {
            case Qt::DisplayRole:
            case FileNameRole:
                return match.fileName;
            case Qt::ToolTipRole:
            case RelativePathRole:
                return match.relativePath;
            case DirectoryPathRole:
                return match.directoryPath;
            case FileNameMatchesRole:
                return QVariant::fromValue( match.fileNameMatchedPositions );
            case PathMatchesRole:
                return QVariant::fromValue( match.pathMatchedPositions );
            case Qt::AccessibleTextRole:
                return match.directoryPath.isEmpty()
                           ? match.fileName
                           : tr( "%1, 경로 %2" ).arg( match.fileName ).arg( match.directoryPath );
            case Qt::AccessibleDescriptionRole:
                return match.relativePath;
            default:
                return {};
        }
    }

    [[nodiscard]] Qt::ItemFlags flags( const QModelIndex& index ) const override
    {
        return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

    [[nodiscard]] bool canFetchMore( const QModelIndex& parent ) const override
    {
        return !parent.isValid() && visibleCount_ < matches_->size();
    }

    void fetchMore( const QModelIndex& parent ) override
    {
        if( parent.isValid() || visibleCount_ >= matches_->size() )
            return;

        const int added = (std::min)( kPageSize,
                                      static_cast<int>( matches_->size() ) - visibleCount_ );
        beginInsertRows( {}, visibleCount_, visibleCount_ + added - 1 );
        visibleCount_ += added;
        endInsertRows();
    }

    [[nodiscard]] MatchListPtr setMatches( MatchList matches )
    {
        return setSharedMatches( std::make_shared< const MatchList >( std::move( matches ) ) );
    }

    [[nodiscard]] MatchListPtr setSharedMatches( MatchListPtr matches )
    {
        if( matches == nullptr )
            matches = std::make_shared< const MatchList >();
        beginResetModel();
        MatchListPtr retired = std::exchange( matches_, std::move( matches ) );
        visibleCount_ = (std::min)( kPageSize, static_cast<int>( matches_->size() ) );
        endResetModel();
        return retired;
    }

    [[nodiscard]] int totalCount() const noexcept
    {
        return static_cast<int>( matches_->size() );
    }

    [[nodiscard]] MatchListPtr sharedMatches() const noexcept
    {
        return matches_;
    }

    [[nodiscard]] QString relativePathAt( const QModelIndex& index ) const
    {
        return index.isValid() ? data( index, RelativePathRole ).toString() : QString{};
    }

    [[nodiscard]] int findPath( const QString& relativePath ) const
    {
        if( relativePath.isEmpty() )
            return -1;
        const QString wanted = pathKey( relativePath );
        const int searchLimit = (std::min)( visibleCount_, static_cast<int>( matches_->size() ) );
        for( int i = 0; i < searchLimit; ++i )
        {
            if( pathKey( matches_->at( i ).match.relativePath ) == wanted )
                return i;
        }
        return -1;
    }

    void revealRow( const int row )
    {
        if( row < visibleCount_ || row < 0 || row >= matches_->size() )
            return;

        const int oldCount = visibleCount_;
        const int newCount = (std::min)( static_cast<int>( matches_->size() ),
                                         ( row / kPageSize + 1 ) * kPageSize );
        beginInsertRows( {}, oldCount, newCount - 1 );
        visibleCount_ = newCount;
        endInsertRows();
    }

private:
    MatchListPtr matches_ = std::make_shared< const MatchList >();
    int          visibleCount_ = 0;
};

namespace {

class QuickOpenItemDelegate final : public QStyledItemDelegate
{
    Q_DECLARE_TR_FUNCTIONS( mrst::QuickOpenItemDelegate )

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint( QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index ) const override
    {
        if( painter == nullptr )
            return;

        QStyleOptionViewItem styled = option;
        initStyleOption( &styled, index );
        styled.text.clear();
        styled.icon = {};
        styled.features &= ~( QStyleOptionViewItem::HasDisplay
                              | QStyleOptionViewItem::HasDecoration );

        const QWidget* widget = option.widget;
        QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl( QStyle::CE_ItemViewItem, &styled, painter, widget );

        const bool selected = ( option.state & QStyle::State_Selected ) != 0;
        const bool enabled = ( option.state & QStyle::State_Enabled ) != 0;
        const QPalette::ColorGroup group = enabled ? QPalette::Active : QPalette::Disabled;
        const QColor primaryColor = option.palette.color(
            group, selected ? QPalette::HighlightedText : QPalette::Text );
        const QColor secondaryColor = option.palette.color(
            group, selected ? QPalette::HighlightedText : QPalette::PlaceholderText );
        const QColor matchColor = selected ? primaryColor
                                           : option.palette.color( group, QPalette::Link );

        const int horizontalMargin = style->pixelMetric( QStyle::PM_FocusFrameHMargin,
                                                         nullptr, widget ) + 8;
        const int iconExtent = style->pixelMetric( QStyle::PM_SmallIconSize, nullptr, widget );
        const int iconGap = 8;
        QRect contents = option.rect.adjusted( horizontalMargin, 5, -horizontalMargin, -5 );
        QRect iconRect;
        QRect textRect = contents;
        if( option.direction == Qt::RightToLeft )
        {
            iconRect = QRect( contents.right() - iconExtent + 1,
                              contents.center().y() - iconExtent / 2,
                              iconExtent, iconExtent );
            textRect.setRight( iconRect.left() - iconGap );
        }
        else
        {
            iconRect = QRect( contents.left(), contents.center().y() - iconExtent / 2,
                              iconExtent, iconExtent );
            textRect.setLeft( iconRect.right() + iconGap );
        }

        const QIcon fileIcon = style->standardIcon( QStyle::SP_FileIcon, nullptr, widget );
        fileIcon.paint( painter, iconRect, Qt::AlignCenter,
                        enabled ? ( selected ? QIcon::Selected : QIcon::Normal )
                                : QIcon::Disabled );

        QFont fileFont = option.font;
        fileFont.setBold( true );
        QFont pathFont = option.font;
        const qreal pointSize = pathFont.pointSizeF();
        if( pointSize > 0.0 )
            pathFont.setPointSizeF( (std::max)( 1.0, pointSize * 0.9 ) );

        const QString fileName = index.data( QuickOpenListModel::FileNameRole ).toString();
        const QString directory = index.data( QuickOpenListModel::DirectoryPathRole ).toString();
        const QVector<int> fileMatches =
            index.data( QuickOpenListModel::FileNameMatchesRole ).value<QVector<int>>();
        QVector<int> pathMatches =
            index.data( QuickOpenListModel::PathMatchesRole ).value<QVector<int>>();
        pathMatches.erase( std::remove_if( pathMatches.begin(), pathMatches.end(),
                                           [ &directory ]( const int position ) {
                                               return position < 0 || position >= directory.size();
                                           } ),
                           pathMatches.end() );

        const QFontMetrics fileMetrics( fileFont );
        const QFontMetrics pathMetrics( pathFont );
        const int columnGap = 16;
        const int pathWidth = (std::clamp)(
            textRect.width() * 46 / 100, 80,
            (std::max)( 80, textRect.width() - 140 - columnGap ) );
        QRect fileRect;
        QRect pathRect;
        if( option.direction == Qt::RightToLeft )
        {
            pathRect = QRect( textRect.left(), textRect.top(), pathWidth, textRect.height() );
            fileRect = QRect( pathRect.right() + 1 + columnGap, textRect.top(),
                              (std::max)( 0, textRect.right() - pathRect.right() - columnGap ),
                              textRect.height() );
        }
        else
        {
            pathRect = QRect( textRect.right() - pathWidth + 1, textRect.top(),
                              pathWidth, textRect.height() );
            fileRect = QRect( textRect.left(), textRect.top(),
                              (std::max)( 0, pathRect.left() - textRect.left() - columnGap ),
                              textRect.height() );
        }

        const ElidedRun shownFile = elideKeepingMatches( fileName, fileMatches, fileMetrics,
                                                         fileRect.width(), Qt::ElideRight );
        const QString shownDirectory = directory.isEmpty() ? tr( "워크스페이스 루트" )
                                                            : directory;
        const ElidedRun shownPath = elideKeepingMatches(
            shownDirectory, directory.isEmpty() ? QVector<int>{} : pathMatches, pathMetrics,
            pathRect.width(), Qt::ElideMiddle );

        painter->save();
        painter->setClipRect( option.rect );
        drawTextRun( painter, fileRect, shownFile.text, fileFont, primaryColor, matchColor,
                     shownFile.matchedPositions, false, option.direction,
                     option.direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft );
        drawTextRun( painter, pathRect, shownPath.text, pathFont, secondaryColor, matchColor,
                     shownPath.matchedPositions, true, option.direction,
                     option.direction == Qt::RightToLeft ? Qt::AlignLeft : Qt::AlignRight );
        painter->restore();
    }

    [[nodiscard]] QSize sizeHint( const QStyleOptionViewItem& option,
                                  const QModelIndex& index ) const override
    {
        Q_UNUSED( index );
        return { 320, (std::max)( 32, QFontMetrics( option.font ).height() + 12 ) };
    }
};

}  // namespace

QuickOpenDialog::QuickOpenDialog( QWidget* parent )
    : QDialog( parent, Qt::Dialog | Qt::FramelessWindowHint )
{
    buildUi();
    retranslateUi();
}

QuickOpenDialog::~QuickOpenDialog()
{
    rankingTimer_->stop();
    footerTimer_->stop();
    rankingStopSource_.request_stop();
    if( rankingPool_ != nullptr )
    {
        rankingPool_->clear();
        rankingPool_->waitForDone();
    }
}

void QuickOpenDialog::buildUi()
{
    setObjectName( QStringLiteral( "quickOpenDialog" ) );
    setModal( true );
    setWindowModality( Qt::WindowModal );
    setMinimumSize( kDialogMinimumWidth, kDialogMinimumHeight );
    resize( kDialogPreferredWidth, kDialogPreferredHeight );
    setSizeGripEnabled( false );

    // worker가 결과를 게시할 때 dialog 자체를 invokeMethod의 context로 쓰면
    // QPointer 확인과 raw pointer 사용 사이에 소멸할 수 있다. 이 객체는 shared
    // ownership으로 게시 이벤트까지 살아 있고 GUI affinity만 가진다.
    guiDispatcher_ = std::make_shared<QObject>();

    // 앱 종료 시 global pool을 clear했다가 종료가 취소되어도 랭킹 runnable이
    // 사라지지 않게 전용 직렬 pool을 쓴다.
    rankingPool_ = new QThreadPool( this );
    rankingPool_->setMaxThreadCount( 1 );
    rankingPool_->setExpiryTimeout( 30'000 );
    rankingPool_->setThreadPriority( QThread::LowPriority );

    auto* outerLayout = new QVBoxLayout( this );
    outerLayout->setContentsMargins( 0, 0, 0, 0 );
    outerLayout->setSpacing( 0 );

    panel_ = new QFrame( this );
    auto* panelFrame = static_cast<QFrame*>( panel_ );
    panelFrame->setFrameShape( QFrame::StyledPanel );
    panelFrame->setFrameShadow( QFrame::Raised );
    panelFrame->setAutoFillBackground( true );
    outerLayout->addWidget( panel_ );

    auto* content = new QVBoxLayout( panel_ );
    content->setContentsMargins( 14, 10, 14, 10 );
    content->setSpacing( 8 );

    auto* header = new QuickOpenHeader( panel_ );
    header->setFrameShape( QFrame::NoFrame );
    auto* headerLayout = new QHBoxLayout( header );
    headerLayout->setContentsMargins( 2, 0, 0, 0 );
    headerLayout->setSpacing( 8 );

    titleLabel_ = new QLabel( header );
    QFont titleFont = titleLabel_->font();
    titleFont.setBold( true );
    titleLabel_->setFont( titleFont );
    headerLayout->addWidget( titleLabel_ );
    headerLayout->addStretch( 1 );

    shortcutLabel_ = new QLabel( header );
    shortcutLabel_->setObjectName( QStringLiteral( "quickOpenShortcut" ) );
    shortcutLabel_->setTextFormat( Qt::PlainText );
    QPalette shortcutPalette = shortcutLabel_->palette();
    shortcutPalette.setColor( QPalette::WindowText,
                              shortcutPalette.color( QPalette::PlaceholderText ) );
    shortcutLabel_->setPalette( shortcutPalette );
    headerLayout->addWidget( shortcutLabel_ );

    closeButton_ = new QToolButton( header );
    closeButton_->setObjectName( QStringLiteral( "quickOpenClose" ) );
    closeButton_->setAutoRaise( true );
    closeButton_->setIcon( style()->standardIcon( QStyle::SP_TitleBarCloseButton ) );
    headerLayout->addWidget( closeButton_ );
    content->addWidget( header );

    auto* edit = new QuickOpenSearchEdit( panel_ );
    searchEdit_ = edit;
    searchEdit_->setObjectName( QStringLiteral( "quickOpenQuery" ) );
    searchEdit_->setMaxLength( kQuickOpenMaximumQueryLength );
    searchEdit_->setClearButtonEnabled( true );
    searchEdit_->installEventFilter( this );
    content->addWidget( searchEdit_ );

    auto* resultArea = new QWidget( panel_ );
    auto* resultStack = new QStackedLayout( resultArea );
    resultStack->setContentsMargins( 0, 0, 0, 0 );
    resultStack->setStackingMode( QStackedLayout::StackAll );

    resultView_ = new QListView( resultArea );
    resultView_->setObjectName( QStringLiteral( "quickOpenResults" ) );
    resultModel_ = new QuickOpenListModel( resultView_ );
    resultView_->setModel( resultModel_ );
    resultView_->setItemDelegate( new QuickOpenItemDelegate( resultView_ ) );
    resultView_->setSelectionMode( QAbstractItemView::SingleSelection );
    resultView_->setSelectionBehavior( QAbstractItemView::SelectRows );
    resultView_->setEditTriggers( QAbstractItemView::NoEditTriggers );
    resultView_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    resultView_->setVerticalScrollMode( QAbstractItemView::ScrollPerPixel );
    resultView_->setUniformItemSizes( true );
    resultView_->setAlternatingRowColors( false );
    resultView_->installEventFilter( this );
    resultStack->addWidget( resultView_ );

    emptyLabel_ = new QLabel( resultArea );
    emptyLabel_->setAlignment( Qt::AlignCenter );
    emptyLabel_->setWordWrap( true );
    emptyLabel_->setAttribute( Qt::WA_TransparentForMouseEvents );
    QPalette emptyPalette = emptyLabel_->palette();
    emptyPalette.setColor( QPalette::WindowText,
                           emptyPalette.color( QPalette::PlaceholderText ) );
    emptyLabel_->setPalette( emptyPalette );
    resultStack->addWidget( emptyLabel_ );
    content->addWidget( resultArea, 1 );

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins( 2, 0, 2, 0 );
    footer->setSpacing( 12 );
    statusLabel_ = new QLabel( panel_ );
    statusLabel_->setObjectName( QStringLiteral( "quickOpenStatus" ) );
    statusLabel_->setTextFormat( Qt::PlainText );
    footer->addWidget( statusLabel_, 1 );

    helpLabel_ = new QLabel( panel_ );
    helpLabel_->setTextFormat( Qt::PlainText );
    footer->addWidget( helpLabel_, 0, Qt::AlignRight );
    content->addLayout( footer );

    for( QLabel* label : { statusLabel_, helpLabel_ } )
    {
        QFont footerFont = label->font();
        if( footerFont.pointSizeF() > 0.0 )
            footerFont.setPointSizeF( (std::max)( 1.0, footerFont.pointSizeF() * 0.9 ) );
        label->setFont( footerFont );
        QPalette footerPalette = label->palette();
        footerPalette.setColor( QPalette::WindowText,
                                footerPalette.color( QPalette::PlaceholderText ) );
        label->setPalette( footerPalette );
    }

    rankingTimer_ = new QTimer( this );
    rankingTimer_->setSingleShot( true );
    rankingTimer_->setInterval( kRankingDebounce );

    footerTimer_ = new QTimer( this );
    footerTimer_->setSingleShot( true );
    footerTimer_->setInterval( kFooterUpdateThrottle );

    setTabOrder( searchEdit_, resultView_ );
    setTabOrder( resultView_, closeButton_ );
    setTabOrder( closeButton_, searchEdit_ );

    connect( closeButton_, &QToolButton::clicked, this, &QDialog::reject );
    connect( searchEdit_, &QLineEdit::textChanged, this,
             [ this ]( const QString& ) { scheduleUserRanking(); } );
    connect( resultView_, &QListView::doubleClicked, this,
             [ this ]( const QModelIndex& ) { chooseCurrent(); } );
    connect( rankingTimer_, &QTimer::timeout, this, &QuickOpenDialog::beginRanking );
    connect( footerTimer_, &QTimer::timeout, this, &QuickOpenDialog::updateFooter );
    connect( resultModel_, &QAbstractItemModel::modelReset, this,
             &QuickOpenDialog::updateFooter );
    connect( resultModel_, &QAbstractItemModel::rowsInserted, this,
             [ this ] { updateFooter(); } );
    connect( resultView_->verticalScrollBar(), &QScrollBar::valueChanged, this,
             [ this ] { prefetchIfNeeded(); } );
    connect( resultView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
             [ this ] { prefetchIfNeeded(); } );
    connect( this, &QDialog::finished, this, [ this ] {
        rankingTimer_->stop();
        footerTimer_->stop();
        ++rankingGeneration_;
        rankingDirty_ = false;
        rankingRequestIsIncremental_ = false;
        rankingThrottleClock_.invalidate();
        rankingStopSource_.request_stop();

        // 이 dialog는 MainWindow 수명 동안 재사용된다. 닫힌 뒤 전체 인덱스와
        // 랭킹 결과를 계속 들고 있으면 PathIndex 캐시만큼 메모리를 중복 점유한다.
        workspaceRoot_.clear();
        shortcutNativeText_.clear();
        resetPathChunks( {} );
        recentRelativePaths_ = {};
        indexedPathKeys_ = {};
        indexedPathKeysValid_ = true;
        indexedTotal_ = 0;
        indexing_ = false;
        openErrorVisible_ = false;
        {
            const QSignalBlocker blocker( searchEdit_ );
            searchEdit_->clear();
        }
        retireMatchesInBackground( resultModel_->setMatches( {} ) );
        updateShortcutLabel();
    } );
}

void QuickOpenDialog::retranslateUi()
{
    setWindowTitle( tr( "파일 빠르게 열기" ) );
    titleLabel_->setText( tr( "파일 빠르게 열기" ) );
    searchEdit_->setPlaceholderText(
        tr( "파일 이름 또는 경로 검색 (예: src/main, docs\\api)" ) );
    searchEdit_->setAccessibleName( tr( "파일 이름 또는 경로" ) );
    searchEdit_->setAccessibleDescription(
        tr( "슬래시와 역슬래시를 모두 경로 구분자로 사용할 수 있습니다." ) );
    resultView_->setAccessibleName( tr( "파일 검색 결과" ) );
    resultView_->setAccessibleDescription(
        tr( "위아래 방향키와 Page Up, Page Down 키로 결과를 선택합니다." ) );
    closeButton_->setToolTip( tr( "닫기" ) );
    closeButton_->setAccessibleName( tr( "빠른 파일 열기 닫기" ) );
    statusLabel_->setAccessibleName( tr( "파일 인덱싱 및 검색 상태" ) );
    helpLabel_->setText( tr( "↑↓ 선택 · Enter 열기 · Esc 닫기" ) );
    helpLabel_->setAccessibleName( tr( "키보드 도움말" ) );
    updateShortcutLabel();
    updateFooter();
}

void QuickOpenDialog::changeEvent( QEvent* event )
{
    if( event != nullptr && event->type() == QEvent::LanguageChange )
        retranslateUi();
    QDialog::changeEvent( event );
}

void QuickOpenDialog::showForWorkspace( const QString& workspaceRoot,
                                        const QStringList& paths,
                                        const bool indexing,
                                        const QStringList& recentRelativePaths,
                                        const QString& shortcutNativeText )
{
    recoverCanceledRankingForShow();
    workspaceRoot_ = QDir( workspaceRoot ).absolutePath();
    indexing_ = indexing;

    normalizeAndReplacePaths( paths );
    indexedTotal_ = indexedPathCount_;
    showPreparedWorkspace( recentRelativePaths, shortcutNativeText );
}

void QuickOpenDialog::showForPathIndex( const QString& workspaceRoot,
                                        const QStringList& normalizedRelativePaths,
                                        const bool indexing,
                                        const QStringList& recentRelativePaths,
                                        const QString& shortcutNativeText )
{
    recoverCanceledRankingForShow();
    workspaceRoot_ = QDir( workspaceRoot ).absolutePath();
    indexing_ = indexing;
    assignPathIndexSnapshot( normalizedRelativePaths );
    indexedTotal_ = indexedPathCount_;
    showPreparedWorkspace( recentRelativePaths, shortcutNativeText );
}

void QuickOpenDialog::showForPathIndexChunks(
    const QString& workspaceRoot,
    const QuickOpenPathChunks& normalizedRelativePathChunks,
    const bool indexing, const qsizetype scannedPathCount,
    const QStringList& recentRelativePaths, const QString& shortcutNativeText )
{
    recoverCanceledRankingForShow();
    workspaceRoot_ = QDir( workspaceRoot ).absolutePath();
    indexing_ = indexing;
    assignPathIndexChunks( normalizedRelativePathChunks );
    indexedTotal_ = (std::max)( scannedPathCount, indexedPathCount_ );
    showPreparedWorkspace( recentRelativePaths, shortcutNativeText );
}

void QuickOpenDialog::showPreparedWorkspace( const QStringList& recentRelativePaths,
                                              const QString& shortcutNativeText )
{
    openErrorVisible_ = false;
    shortcutNativeText_ = shortcutNativeText;
    updateShortcutLabel();

    recentRelativePaths_.clear();
    QSet<QString> recentKeys;
    for( const QString& path : recentRelativePaths )
    {
        const QString normalized = normalizeIncomingPath( path );
        const QString key = pathKey( normalized );
        if( !normalized.isEmpty() && !recentKeys.contains( key ) )
        {
            recentKeys.insert( key );
            recentRelativePaths_.append( normalized );
        }
    }

    {
        const QSignalBlocker blocker( searchEdit_ );
        searchEdit_->clear();
    }
    retireMatchesInBackground( resultModel_->setMatches( {} ) );
    scheduleRanking();

    show();
    raise();
    activateWindow();
    searchEdit_->setFocus( Qt::ShortcutFocusReason );
    searchEdit_->selectAll();
}

void QuickOpenDialog::replaceIndexedPaths( const QStringList& paths )
{
    normalizeAndReplacePaths( paths );
    indexedTotal_ = indexedPathCount_;
    scheduleRanking();
    updateFooter();
}

void QuickOpenDialog::appendIndexedPaths( const QStringList& batch, const qsizetype indexedTotal )
{
    ensureIndexedPathKeys();
    QStringList normalizedBatch;
    normalizedBatch.reserve( batch.size() );
    for( const QString& path : batch )
    {
        const QString normalized = normalizeIncomingPath( path );
        const QString key = pathKey( normalized );
        if( normalized.isEmpty() || indexedPathKeys_.contains( key ) )
            continue;
        indexedPathKeys_.insert( key );
        normalizedBatch.append( normalized );
    }
    const bool changed = !normalizedBatch.isEmpty();
    appendPathChunk( std::move( normalizedBatch ) );

    indexing_ = true;
    indexedTotal_ = indexedTotal >= 0 ? (std::max)(
                                              indexedTotal,
                                              indexedPathCount_ )
                                      : indexedPathCount_;
    if( changed )
        scheduleIncrementalRanking();
    scheduleFooterUpdate();
}

void QuickOpenDialog::replacePathIndexSnapshot(
    const QStringList& normalizedRelativePaths )
{
    assignPathIndexSnapshot( normalizedRelativePaths );
    indexedTotal_ = indexedPathCount_;
    scheduleIncrementalRanking();
    updateFooter();
}

void QuickOpenDialog::replacePathIndexChunks(
    const QuickOpenPathChunks& normalizedRelativePathChunks,
    const qsizetype scannedPathCount )
{
    assignPathIndexChunks( normalizedRelativePathChunks );
    indexing_ = true;
    indexedTotal_ = (std::max)( scannedPathCount, indexedPathCount_ );
    scheduleIncrementalRanking();
    updateFooter();
}

void QuickOpenDialog::appendPathIndexBatch( const QStringList& normalizedBatch,
                                            const qsizetype indexedTotal )
{
    if( !normalizedBatch.isEmpty() )
    {
        appendPathChunk( normalizedBatch );
        if( indexedPathKeysValid_ )
        {
            indexedPathKeys_.clear();
            indexedPathKeysValid_ = false;
        }
    }
    indexing_ = true;
    indexedTotal_ = (std::max)( indexedTotal,
                                indexedPathCount_ );
    if( !normalizedBatch.isEmpty() )
        scheduleIncrementalRanking();
    scheduleFooterUpdate();
}

void QuickOpenDialog::setIndexingProgress( const qsizetype indexedTotal )
{
    indexing_ = true;
    indexedTotal_ = (std::max)( static_cast<qsizetype>( 0 ), indexedTotal );
    scheduleFooterUpdate();
}

void QuickOpenDialog::finishIndexing()
{
    indexing_ = false;
    indexedTotal_ = indexedPathCount_;
    footerTimer_->stop();
    updateFooter();
}

void QuickOpenDialog::finishIndexing( const QStringList& finalPaths )
{
    normalizeAndReplacePaths( finalPaths );
    indexing_ = false;
    indexedTotal_ = indexedPathCount_;
    scheduleIncrementalRanking();
    footerTimer_->stop();
    updateFooter();
}

void QuickOpenDialog::finishPathIndexing( const QStringList& normalizedFinalPaths )
{
    assignPathIndexSnapshot( normalizedFinalPaths );
    indexing_ = false;
    indexedTotal_ = indexedPathCount_;
    scheduleIncrementalRanking();
    footerTimer_->stop();
    updateFooter();
}

void QuickOpenDialog::normalizeAndReplacePaths( const QStringList& paths )
{
    QStringList normalizedPaths;
    normalizedPaths.reserve( paths.size() );
    QSet<QString> keys;
    keys.reserve( paths.size() );

    for( const QString& path : paths )
    {
        const QString normalized = normalizeIncomingPath( path );
        const QString key = pathKey( normalized );
        if( normalized.isEmpty() || keys.contains( key ) )
            continue;
        keys.insert( key );
        normalizedPaths.append( normalized );
    }

    resetPathChunks( std::move( normalizedPaths ) );
    indexedPathKeys_ = std::move( keys );
    indexedPathKeysValid_ = true;
}

void QuickOpenDialog::assignPathIndexSnapshot( const QStringList& paths )
{
    // PathIndex가 보장한 정규화 결과를 하나의 불변 chunk로 유지한다. 이후
    // batch는 별도 chunk가 되므로 worker snapshot과 공유 중이어도 전체 목록을
    // detach하지 않는다.
    resetPathChunks( paths );
    indexedPathKeys_.clear();
    indexedPathKeysValid_ = false;
}

void QuickOpenDialog::assignPathIndexChunks( const QuickOpenPathChunks& paths )
{
    QuickOpenPathChunks retired = std::move( indexedPathChunks_ );
    indexedPathChunks_ = paths;
    indexedPathCount_ = 0;
    for( const QuickOpenPathChunk& chunk : std::as_const( indexedPathChunks_ ) )
    {
        if( chunk != nullptr )
            indexedPathCount_ += chunk->size();
    }

    retireQuickOpenPathChunks( std::move( retired ) );

    indexedPathKeys_.clear();
    indexedPathKeysValid_ = false;
    ++pathSnapshotGeneration_;
    appliedRankingValid_ = false;
}

void QuickOpenDialog::ensureIndexedPathKeys()
{
    if( indexedPathKeysValid_ )
        return;

    indexedPathKeys_.clear();
    indexedPathKeys_.reserve( indexedPathCount_ );
    for( const QuickOpenPathChunk& chunk : std::as_const( indexedPathChunks_ ) )
    {
        if( chunk == nullptr )
            continue;
        for( const QString& path : *chunk )
            indexedPathKeys_.insert( pathKey( path ) );
    }
    indexedPathKeysValid_ = true;
}

void QuickOpenDialog::appendPathChunk( QStringList paths )
{
    if( paths.isEmpty() )
        return;

    indexedPathCount_ += paths.size();
    indexedPathChunks_.append(
        std::make_shared< const QStringList >( std::move( paths ) ) );
}

void QuickOpenDialog::resetPathChunks( QStringList paths )
{
    QuickOpenPathChunks retired = std::move( indexedPathChunks_ );
    indexedPathChunks_.clear();
    indexedPathCount_ = 0;
    appendPathChunk( std::move( paths ) );
    ++pathSnapshotGeneration_;
    appliedRankingValid_ = false;

    retireQuickOpenPathChunks( std::move( retired ) );
}

void QuickOpenDialog::scheduleFooterUpdate()
{
    if( footerTimer_ != nullptr && !footerTimer_->isActive() )
        footerTimer_->start();
}

void QuickOpenDialog::retireMatchesInBackground(
    std::shared_ptr< const QuickOpenRankedMatches > matches )
{
    retireQuickOpenMatches( std::move( matches ) );
}

QString QuickOpenDialog::normalizeIncomingPath( const QString& path ) const
{
    QString candidate = QDir::fromNativeSeparators( path );
    if( candidate.isEmpty() )
        return {};

    if( QDir::isAbsolutePath( candidate ) )
    {
        if( workspaceRoot_.isEmpty() )
            return {};
        candidate = QDir( workspaceRoot_ ).relativeFilePath( QDir::cleanPath( candidate ) );
    }

    candidate = QDir::cleanPath( candidate );
    if( candidate == QLatin1String( ".." ) || candidate.startsWith( QLatin1String( "../" ) )
        || QDir::isAbsolutePath( candidate ) )
    {
        return {};
    }

    const QString normalized = normalizeQuickOpenPath( candidate );
    return normalized == QLatin1String( "." ) ? QString{} : normalized;
}

void QuickOpenDialog::scheduleRanking()
{
    openErrorVisible_ = false;
    ++rankingGeneration_;
    rankingDirty_ = true;
    rankingRequestIsIncremental_ = false;
    if( rankingInFlight_ )
        rankingStopSource_.request_stop();
    rankingTimer_->start( kRankingDebounce );
}

void QuickOpenDialog::scheduleUserRanking()
{
    recordQueryInputActivity();
    scheduleRanking();
}

QString QuickOpenDialog::currentQuery() const
{
    const auto* edit = static_cast<const QuickOpenSearchEdit*>( searchEdit_ );
    return edit != nullptr ? edit->effectiveSearchText() : QString{};
}

void QuickOpenDialog::recordQueryInputActivity()
{
    queryInputClock_.restart();
    if( rankingRequestIsIncremental_ && rankingTimer_->isActive() )
        armPendingRankingTimer();
}

void QuickOpenDialog::scheduleIncrementalRanking()
{
    // 인덱스 batch는 이미 계산 중인 부분 결과를 무효화하거나 취소하지 않는다.
    // 첫 batch에서만 타이머를 시작해 연속 입력에도 최대 대기 시간이 늘어나지
    // 않고, 계산이 끝나면 dirty 최신 snapshot을 다음 주기에 반영한다.
    openErrorVisible_ = false;
    if( !rankingDirty_ )
        rankingRequestIsIncremental_ = true;
    rankingDirty_ = true;
    if( !rankingInFlight_ && !rankingTimer_->isActive() )
        armPendingRankingTimer();
}

void QuickOpenDialog::armPendingRankingTimer()
{
    auto delay = kRankingDebounce;
    if( rankingRequestIsIncremental_ && rankingThrottleClock_.isValid() )
    {
        const auto elapsed = std::chrono::milliseconds{ rankingThrottleClock_.elapsed() };
        delay = (std::max)( delay, kIncrementalRankingThrottle -
                                      (std::min)( elapsed, kIncrementalRankingThrottle ) );
    }
    if( rankingRequestIsIncremental_ && queryInputClock_.isValid() )
    {
        const auto elapsed = std::chrono::milliseconds{ queryInputClock_.elapsed() };
        delay = (std::max)( delay, kIncrementalRankingInputIdle -
                                      (std::min)( elapsed, kIncrementalRankingInputIdle ) );
    }
    rankingTimer_->start( delay );
}

void QuickOpenDialog::beginRanking()
{
    if( rankingInFlight_ || !rankingDirty_ )
        return;

    const auto* edit = static_cast<const QuickOpenSearchEdit*>( searchEdit_ );
    if( rankingRequestIsIncremental_
        && edit->isComposing() )
    {
        rankingTimer_->start( kIncrementalRankingInputIdle );
        return;
    }
    if( rankingRequestIsIncremental_ && queryInputClock_.isValid()
        && std::chrono::milliseconds{ queryInputClock_.elapsed() }
               < kIncrementalRankingInputIdle )
    {
        armPendingRankingTimer();
        return;
    }

    const bool incrementalRequest = rankingRequestIsIncremental_;
    rankingDirty_ = false;
    rankingRequestIsIncremental_ = false;
    rankingInFlight_ = true;
    rankingStopSource_ = std::stop_source{};

    const quint64 generation = rankingGeneration_;
    activeRankingGeneration_ = generation;
    // QVector detach가 복사하는 것은 chunk 포인터뿐이다. 각 QStringList는
    // 게시 뒤 불변이므로 다음 인덱스 batch가 와도 누적 경로 전체가 GUI에서
    // copy-on-write detach되지 않는다.
    const QuickOpenPathChunks paths = indexedPathChunks_;
    const quint64 pathSnapshotGeneration = pathSnapshotGeneration_;
    const qsizetype rankedChunkCount = paths.size();
    const qsizetype rankedPathCount = indexedPathCount_;
    const QString query = currentQuery();
    const QStringList recentPaths = recentRelativePaths_;
    std::shared_ptr< const QuickOpenRankedMatches > existingMatches;
    QuickOpenPathChunks pathsToRank = paths;
    qsizetype inputOrderOffset = 0;
    if( incrementalRequest && appliedRankingValid_
        && appliedPathSnapshotGeneration_ == pathSnapshotGeneration
        && appliedQuery_ == query && appliedChunkCount_ <= paths.size()
        && appliedPathCount_ <= rankedPathCount )
    {
        existingMatches = resultModel_->sharedMatches();
        pathsToRank = paths.mid( appliedChunkCount_ );
        inputOrderOffset = appliedPathCount_;
    }
    const std::stop_token stopToken = rankingStopSource_.get_token();
    const QPointer<QuickOpenDialog> guard( this );
    const std::shared_ptr<QObject> dispatcher = guiDispatcher_;

    rankingPool_->start(
        [ dispatcher, guard, generation, pathsToRank = std::move( pathsToRank ),
          query, recentPaths, existingMatches = std::move( existingMatches ),
          inputOrderOffset, pathSnapshotGeneration, rankedChunkCount,
          rankedPathCount, stopToken ]() mutable {
            std::shared_ptr< const QuickOpenRankedMatches > matches;
            if( existingMatches != nullptr && pathsToRank.isEmpty() )
                matches = existingMatches;
            else
            {
                QuickOpenRankedMatches ranked = rankQuickOpenFileChunksDetailed(
                    pathsToRank, query, recentPaths, inputOrderOffset, stopToken );
                if( existingMatches != nullptr && !stopToken.stop_requested() )
                {
                    ranked = mergeQuickOpenRankedMatches(
                        *existingMatches, std::move( ranked ), query, stopToken );
                }
                matches = std::make_shared< const QuickOpenRankedMatches >(
                    std::move( ranked ) );
            }

            QMetaObject::invokeMethod(
                dispatcher.get(),
                [ dispatcher, guard, generation, pathSnapshotGeneration,
                  rankedChunkCount, rankedPathCount, query,
                  matches = std::move( matches ) ]() mutable {
                    Q_UNUSED( dispatcher );
                    if( guard.isNull() )
                    {
                        retireQuickOpenMatches( std::move( matches ) );
                        return;
                    }

                    // show 중 stale 상태를 복구해 새 작업을 예약했으면 옛 callback이
                    // 그 작업의 in-flight 상태를 false로 덮어쓰면 안 된다.
                    if( generation != guard->activeRankingGeneration_ )
                    {
                        guard->retireMatchesInBackground( std::move( matches ) );
                        return;
                    }

                    guard->activeRankingGeneration_ = 0;
                    guard->rankingInFlight_ = false;
                    guard->rankingThrottleClock_.restart();
                    if( generation == guard->rankingGeneration_
                        && pathSnapshotGeneration == guard->pathSnapshotGeneration_
                        && guard->isVisible() )
                    {
                        const QString selectedPath = guard->resultModel_->relativePathAt(
                            guard->resultView_->currentIndex() );
                        const QModelIndex oldTop = guard->resultView_->indexAt( QPoint( 1, 1 ) );
                        const QString topPath = guard->resultModel_->relativePathAt( oldTop );
                        const int topOffset = oldTop.isValid()
                                                  ? guard->resultView_->visualRect( oldTop ).top()
                                                  : 0;

                        auto retiredMatches = guard->resultModel_->setSharedMatches(
                            std::move( matches ) );
                        guard->retireMatchesInBackground( std::move( retiredMatches ) );
                        guard->appliedPathSnapshotGeneration_ = pathSnapshotGeneration;
                        guard->appliedChunkCount_ = rankedChunkCount;
                        guard->appliedPathCount_ = rankedPathCount;
                        guard->appliedQuery_ = query;
                        guard->appliedRankingValid_ = true;

                        const int topRow = guard->resultModel_->findPath( topPath );
                        if( topRow >= 0 )
                        {
                            guard->resultModel_->revealRow( topRow );
                            const QModelIndex restoredTop = guard->resultModel_->index( topRow, 0 );
                            guard->resultView_->scrollTo( restoredTop,
                                                         QAbstractItemView::PositionAtTop );
                            guard->resultView_->verticalScrollBar()->setValue(
                                guard->resultView_->verticalScrollBar()->value() - topOffset );
                        }
                        else
                        {
                            guard->resultView_->scrollToTop();
                        }

                        int selectedRow = guard->resultModel_->findPath( selectedPath );
                        if( selectedRow < 0 && guard->resultModel_->rowCount() > 0 )
                            selectedRow = 0;
                        if( selectedRow >= 0 )
                        {
                            guard->resultModel_->revealRow( selectedRow );
                            const QModelIndex restoredSelection =
                                guard->resultModel_->index( selectedRow, 0 );
                            guard->resultView_->selectionModel()->setCurrentIndex(
                                restoredSelection,
                                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows );
                            if( !guard->resultView_->viewport()->rect().intersects(
                                    guard->resultView_->visualRect( restoredSelection ) ) )
                            {
                                guard->resultView_->scrollTo(
                                    restoredSelection, QAbstractItemView::EnsureVisible );
                            }
                        }
                        guard->updateFooter();
                    }
                    else
                    {
                        guard->retireMatchesInBackground( std::move( matches ) );
                    }

                    if( guard->rankingDirty_ && guard->isVisible()
                        && !guard->rankingTimer_->isActive() )
                    {
                        if( guard->rankingRequestIsIncremental_ )
                            guard->armPendingRankingTimer();
                        else
                            guard->beginRanking();
                    }
                },
                Qt::QueuedConnection );
        } );
}

void QuickOpenDialog::recoverCanceledRankingForShow()
{
    // 닫힐 때 취소된 작업의 callable이 외부 pool clear 등으로 실행되지 않았던
    // 경우에도 새 show가 영원히 in-flight 상태를 물려받지 않는다. 전용 pool은
    // 옛 runnable을 보존하며, generation gate가 늦은 callback을 격리한다.
    if( rankingInFlight_ && rankingStopSource_.stop_requested() )
    {
        activeRankingGeneration_ = 0;
        rankingInFlight_ = false;
    }
}

void QuickOpenDialog::chooseCurrent()
{
    const QString relativePath = resultModel_->relativePathAt( resultView_->currentIndex() );
    if( relativePath.isEmpty() || workspaceRoot_.isEmpty() )
        return;

    const QString absolutePath = QDir::cleanPath(
        QDir( workspaceRoot_ ).absoluteFilePath( relativePath ) );

    // 색인 뒤 파일이나 부모 디렉터리가 junction/symlink로 바뀔 수 있다.
    // 선택 시점의 canonical 경로로 다시 확인해 workspace 경계를 넘는 TOCTOU를
    // 막고, 사라졌거나 디렉터리로 바뀐 항목도 열지 않는다.
    const QFileInfo rootInfo( workspaceRoot_ );
    const QFileInfo fileInfo( absolutePath );
    QString canonicalRoot = QDir::fromNativeSeparators( rootInfo.canonicalFilePath() );
    const QString canonicalFile = QDir::fromNativeSeparators( fileInfo.canonicalFilePath() );
    if( canonicalRoot.isEmpty() || canonicalFile.isEmpty() || !fileInfo.exists()
        || !fileInfo.isFile() )
    {
        openErrorVisible_ = true;
        updateFooter();
        QAccessibleEvent accessibleEvent( statusLabel_, QAccessible::Alert );
        QAccessible::updateAccessibility( &accessibleEvent );
        return;
    }
    if( !canonicalRoot.endsWith( QLatin1Char( '/' ) ) )
        canonicalRoot.append( QLatin1Char( '/' ) );
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    if( !canonicalFile.startsWith( canonicalRoot, pathCase ) )
    {
        openErrorVisible_ = true;
        updateFooter();
        QAccessibleEvent accessibleEvent( statusLabel_, QAccessible::Alert );
        QAccessible::updateAccessibility( &accessibleEvent );
        return;
    }

    accept();
    emit fileChosen( QDir::toNativeSeparators( canonicalFile ) );
}

void QuickOpenDialog::moveCurrent( const int rows )
{
    if( resultModel_->rowCount() <= 0 )
        return;

    int currentRow = resultView_->currentIndex().row();
    if( currentRow < 0 )
        currentRow = rows < 0 ? resultModel_->rowCount() - 1 : 0;
    else
        currentRow = (std::clamp)( currentRow + rows, 0,
                                   resultModel_->rowCount() - 1 );

    const QModelIndex next = resultModel_->index( currentRow, 0 );
    resultView_->selectionModel()->setCurrentIndex(
        next, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows );
    resultView_->scrollTo( next, QAbstractItemView::EnsureVisible );
    prefetchIfNeeded();
}

void QuickOpenDialog::prefetchIfNeeded()
{
    if( resultModel_->rowCount() <= 0 || !resultModel_->canFetchMore( {} ) )
        return;

    QModelIndex bottom = resultView_->indexAt(
        QPoint( (std::max)( 0, resultView_->viewport()->width() / 2 ),
                (std::max)( 0, resultView_->viewport()->height() - 1 ) ) );
    if( !bottom.isValid() )
        bottom = resultView_->currentIndex();
    if( bottom.isValid()
        && bottom.row() >= resultModel_->rowCount() - kPrefetchThreshold - 1 )
    {
        resultModel_->fetchMore( {} );
    }
}

void QuickOpenDialog::updateFooter()
{
    if( resultModel_ == nullptr || statusLabel_ == nullptr || emptyLabel_ == nullptr )
        return;

    const qlonglong indexed = static_cast<qlonglong>( (std::max)(
        indexedTotal_, indexedPathCount_ ) );
    const qlonglong matches = static_cast<qlonglong>( resultModel_->totalCount() );
    const qlonglong visible = static_cast<qlonglong>( resultModel_->rowCount() );
    const QString shown = visible > 0 ? tr( "1–%L1 표시" ).arg( visible )
                                      : tr( "0개 표시" );
    if( openErrorVisible_ )
    {
        const QString error =
            tr( "파일이 없거나 워크스페이스 밖을 가리켜 열 수 없습니다." );
        statusLabel_->setText( error );
        statusLabel_->setAccessibleDescription( error );
    }
    else
    {
        statusLabel_->setText( indexing_
                                   ? tr( "인덱싱 중: %L1개 · %L2개 일치 · %3" )
                                         .arg( indexed ).arg( matches ).arg( shown )
                                   : tr( "%L1개 파일 · %L2개 일치 · %3" )
                                         .arg( indexed ).arg( matches ).arg( shown ) );
        statusLabel_->setAccessibleDescription( {} );
    }

    const bool empty = resultModel_->rowCount() == 0;
    emptyLabel_->setVisible( empty );
    if( empty )
    {
        if( indexing_ && indexedPathCount_ == 0 )
            emptyLabel_->setText( tr( "파일 인덱스를 만드는 중입니다…" ) );
        else if( !currentQuery().isEmpty() )
            emptyLabel_->setText( tr( "일치하는 파일이 없습니다." ) );
        else
            emptyLabel_->setText( tr( "표시할 파일이 없습니다." ) );
    }
}

void QuickOpenDialog::updateShortcutLabel()
{
    if( shortcutLabel_ == nullptr )
        return;

    shortcutLabel_->setText( shortcutNativeText_ );
    shortcutLabel_->setVisible( !shortcutNativeText_.isEmpty() );
    shortcutLabel_->setAccessibleName(
        shortcutNativeText_.isEmpty()
            ? QString{}
            : tr( "빠른 파일 열기 단축키: %1" ).arg( shortcutNativeText_ ) );
}

void QuickOpenDialog::positionOverParent()
{
    const QWidget* anchor = parentWidget() != nullptr ? parentWidget()->window() : nullptr;
    QRect reference;
    if( anchor != nullptr && anchor->isVisible() )
        reference = QRect( anchor->mapToGlobal( QPoint( 0, 0 ) ), anchor->size() );

    QScreen* targetScreen = nullptr;
    if( !reference.isEmpty() )
        targetScreen = QGuiApplication::screenAt( reference.center() );
    if( targetScreen == nullptr )
        targetScreen = screen() != nullptr ? screen() : QGuiApplication::primaryScreen();
    const QRect available = targetScreen != nullptr ? targetScreen->availableGeometry() : reference;
    if( reference.isEmpty() )
        reference = available;
    if( reference.isEmpty() )
        return;

    const int desiredWidth = (std::clamp)( qRound( reference.width() * 0.64 ),
                                           kDialogMinimumWidth, 820 );
    const int desiredHeight = kDialogPreferredHeight;
    const int availableWidth = available.isEmpty() ? desiredWidth
                                                    : (std::max)( kDialogMinimumWidth,
                                                                  available.width() - 32 );
    const int availableHeight = available.isEmpty() ? desiredHeight
                                                     : (std::max)( kDialogMinimumHeight,
                                                                   available.height() - 32 );
    resize( (std::min)( desiredWidth, availableWidth ),
            (std::min)( desiredHeight, availableHeight ) );

    const int verticalSlack = (std::max)( 0, reference.height() - height() );
    QPoint topLeft( reference.center().x() - width() / 2,
                    reference.top() + qRound( verticalSlack * 0.30 ) );
    if( !available.isEmpty() )
    {
        topLeft.setX( (std::clamp)(
            topLeft.x(), available.left(),
            (std::max)( available.left(), available.right() - width() + 1 ) ) );
        topLeft.setY( (std::clamp)(
            topLeft.y(), available.top(),
            (std::max)( available.top(), available.bottom() - height() + 1 ) ) );
    }
    move( topLeft );
}

void QuickOpenDialog::showEvent( QShowEvent* event )
{
    QDialog::showEvent( event );
    positionOverParent();
}

bool QuickOpenDialog::eventFilter( QObject* watched, QEvent* event )
{
    if( watched == searchEdit_ && event != nullptr
        && event->type() == QEvent::InputMethod )
    {
        const auto* inputEvent = static_cast<const QInputMethodEvent*>( event );
        const auto* edit = static_cast<const QuickOpenSearchEdit*>( searchEdit_ );
        if( edit->isComposing() || !inputEvent->preeditString().isEmpty()
            || !inputEvent->commitString().isEmpty() )
        {
            // QLineEdit::text()에는 조합 중 preedit이 들어가지 않는다. 이벤트가
            // 위젯에 적용된 뒤 타이머가 currentQuery()를 읽어 현재 조합 글자까지
            // 포함해 검색한다. 빈 preedit으로 조합이 끝나는 경우도 다시 검색한다.
            scheduleUserRanking();
        }
        return QDialog::eventFilter( watched, event );
    }

    if( event == nullptr || event->type() != QEvent::KeyPress )
        return QDialog::eventFilter( watched, event );

    auto* keyEvent = static_cast<QKeyEvent*>( event );
    if( watched == searchEdit_ )
    {
        const auto* edit = static_cast<const QuickOpenSearchEdit*>( searchEdit_ );
        if( edit->isComposing() )
            return false;

        switch( keyEvent->key() )
        {
            case Qt::Key_Up:
                moveCurrent( -1 );
                return true;
            case Qt::Key_Down:
                moveCurrent( 1 );
                return true;
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
            {
                const int rowHeight = (std::max)(
                    1, resultView_->sizeHintForRow(
                           (std::max)( 0, resultView_->currentIndex().row() ) ) );
                const int pageRows = (std::max)(
                    1, resultView_->viewport()->height() / rowHeight - 1 );
                moveCurrent( keyEvent->key() == Qt::Key_PageUp ? -pageRows : pageRows );
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                chooseCurrent();
                return true;
            case Qt::Key_Escape:
                reject();
                return true;
            default:
                break;
        }
    }
    else if( watched == resultView_ )
    {
        if( keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter )
        {
            chooseCurrent();
            return true;
        }
        if( keyEvent->key() == Qt::Key_Escape )
        {
            reject();
            return true;
        }
    }

    return QDialog::eventFilter( watched, event );
}

}  // namespace mrst

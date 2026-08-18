#include "stdafx.h"
#include "solRestCompletionCoordinator.hpp"

#include "editor/QBaseEditor.hpp"
#include "editor/RstContainerLexer.hpp"
#include "solGlossaryIndex.hpp"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QScopedValueRollback>
#include <QTimer>

namespace mrst {
namespace {

/// 파이썬 원본과 같은 감각. 너무 짧으면 한 글자마다 요청이 날아가고,
/// 너무 길면 팝업이 뒤늦게 뜬다.
constexpr int kDebounceMs = 150;

/// 이 문자를 입력했을 때만 완성을 시작한다. 그 외 글자는 이미 떠 있는 팝업의
/// 필터만 갱신한다. 평범한 산문을 치는 동안 팝업이 튀어나오면 안 된다.
bool isTriggerCharacter( const int character )
{
    switch( character )
    {
        case '.':
        case ':':
        case '`':
        case '/':
        case '\\':
        case '<':
        case '>':
        case ' ':
            return true;
        default:
            return false;
    }
}

/// Esbonio v2 가 등록한 트리거 문자. 그 외에는 triggerKind=1(Invoked) 로 보낸다.
bool isEsbonioTriggerCharacter( const QString& character )
{
    static const QStringList registered{ QStringLiteral( ":" ), QStringLiteral( "/" ),
                                        QStringLiteral( "<" ), QStringLiteral( ">" ),
                                        QStringLiteral( " " ), QStringLiteral( "`" ) };
    return registered.contains( character );
}

CompletionDisplayItem toDisplay( const rstcomplete::Item& item )
{
    return { item.label, item.insertText, item.detail, item.kind, item.label };
}

rstcomplete::Item fromLsp( const LspCompletionItem& item )
{
    return { item.label, item.insertText.isEmpty() ? item.label : item.insertText, item.detail,
            item.kind };
}

QList< CompletionDisplayItem > toDisplayList( const QVector< rstcomplete::Item >& items )
{
    QList< CompletionDisplayItem > display;
    display.reserve( items.size() );
    for( const rstcomplete::Item& item : items )
        display.push_back( toDisplay( item ) );
    return display;
}

}  // namespace

CompletionCoordinator::CompletionCoordinator( QObject* parent )
    : QObject( parent )
    , popup_( new CompletionPopupWidget( nullptr ) )
    , detail_( new CompletionDetailPopup( nullptr ) )
    , debounce_( new QTimer( this ) )
{
    debounce_->setSingleShot( true );
    debounce_->setInterval( kDebounceMs );
    connect( debounce_, &QTimer::timeout, this, &CompletionCoordinator::flushTrigger );

    connect( popup_, &CompletionPopupWidget::itemSelected, this,
            &CompletionCoordinator::insertCompletion );
    connect( popup_, &CompletionPopupWidget::currentItemChanged, this,
            &CompletionCoordinator::refreshDetailPopup );
    // 목록은 Esc 키, 항목 확정, 필터 결과 없음 등 여러 경로로 스스로 숨는다.
    // 그중 Esc 는 여기까지 알려 오는 길이 없어서 오른쪽 상세 패널만 화면에
    // 남아 있었다. 숨김 자체를 신호로 받아 한 곳에서 정리한다.
    connect( popup_, &CompletionPopupWidget::popupHidden, this,
            &CompletionCoordinator::hidePopup );
}

CompletionCoordinator::~CompletionCoordinator()
{
    qApp->removeEventFilter( this );
    delete detail_.data();
    delete popup_.data();
}

void CompletionCoordinator::setGlossaryIndex( GlossaryIndex* glossary )
{
    glossary_ = glossary;
}

void CompletionCoordinator::showPopupAtCaret()
{
    if( popup_.isNull() || activeView_.isNull() || popup_->visibleCount() <= 0 )
        return;

    // 부모 없는 Tool 창은 Windows 에서 메인 창 뒤로 숨는다. 창이 바뀔 때마다
    // 붙여 준다 (탭이 다른 창으로 떨어져 나갈 수 있다).
    QWidget* window = activeView_->window();
    if( popup_->parentWidget() != window )
        popup_->setParent( window, popup_->windowFlags() );
    if( !detail_.isNull() && detail_->parentWidget() != window )
        detail_->setParent( window, detail_->windowFlags() );

    popup_->showAt( activeView_->caretGlobalPos() );
    qApp->installEventFilter( this );

    // 목록이 자리를 잡은 뒤에 옆에 붙인다.
    refreshDetailPopup( popup_->currentItem() );
}

void CompletionCoordinator::showOrRefreshPopup()
{
    if( popup_.isNull() || activeView_.isNull() || popup_->visibleCount() <= 0 )
        return;

    if( !popup_->isVisible() )
    {
        showPopupAtCaret();
        return;
    }

    // 이미 떠 있는 목록을 showAt() 으로 다시 띄우면 그동안 움직인 캐럿을 따라
    // 옆으로 튄다. LSP 응답은 200~500ms 뒤에 오는데, 그 사이에 "../images/" 를
    // 치면 그만큼 점프한다. 경로 완성은 타이핑이 길어 매우 잘 보인다.
    popup_->refreshGeometry();
    refreshDetailPopup( popup_->currentItem() );
}

void CompletionCoordinator::refreshDetailPopup( const CompletionDisplayItem& item )
{
    if( detail_.isNull() )
        return;
    if( popup_.isNull() || !popup_->isVisible() )
    {
        detail_->hide();
        return;
    }

    QString title;
    QString body;
    QString source;
    if( !buildDetailContent( item, &title, &body, &source )
        || !detail_->setContent( title, body, source ) )
    {
        detail_->hide();
        return;
    }

    detail_->showBesideAnchor( popup_->geometry() );
}

bool CompletionCoordinator::buildDetailContent( const CompletionDisplayItem& item, QString* title,
                                                QString* body, QString* source ) const
{
    if( item.label.isEmpty() )
        return false;

    *title = item.label;
    *body = item.detail;
    source->clear();

    // 용어집 항목이면 정의 전문과 출처를 붙인다. 롤 대상 컨텍스트가 아니어도
    // 이름이 용어와 같으면 보여 준다 (LSP 가 준 후보도 마찬가지로 걸린다).
    const bool termContext = shownContext_.kind == rstcomplete::ContextKind::RoleTarget
                             && shownContext_.directiveName == QLatin1String( "term" );
    if( glossary_ != nullptr && ( termContext || shownContext_.kind == rstcomplete::ContextKind::None ) )
    {
        if( const GlossaryEntry* entry = glossary_->lookup( item.label ); entry != nullptr )
        {
            *body = entry->definition;
            if( !entry->path.isEmpty() )
            {
                *source = QStringLiteral( "%1:%2" )
                              .arg( QFileInfo( entry->path ).fileName() )
                              .arg( entry->line );
            }
        }
    }

    return !title->isEmpty() && ( !body->isEmpty() || !source->isEmpty() );
}

QList< CompletionDisplayItem >
CompletionCoordinator::localCandidatesFor( const rstcomplete::Context& context ) const
{
    QList< CompletionDisplayItem > items;
    if( glossary_ == nullptr || context.kind != rstcomplete::ContextKind::RoleTarget )
        return items;
    if( context.directiveName != QLatin1String( "term" ) )
        return items;

    // Esbonio 는 프로젝트 빌드가 끝나야 대상 후보를 만든다. 그 사이에도
    // 용어집만큼은 우리가 직접 훑어 둔 것이 있으므로 바로 쓸 수 있다.
    const QVector< GlossaryEntry > entries = glossary_->match( QString{} );
    items.reserve( static_cast< int >( entries.size() ) );
    for( const GlossaryEntry& entry : entries )
    {
        CompletionDisplayItem item;
        item.label = entry.term;
        item.insertText = entry.term;
        item.detail = QObject::tr( "용어집" );
        item.kind = 21;   // LSP CompletionItemKind::Constant — 다른 롤 대상과 구분만 되면 된다
        item.filterText = entry.term;
        items.push_back( item );
    }
    return items;
}

void CompletionCoordinator::notifyGlossaryReady( const QString& projectId )
{
    if( projectId != activeProjectId_ || !isPopupVisible() || popup_.isNull() )
        return;
    if( shownContext_.kind != rstcomplete::ContextKind::RoleTarget
        || shownContext_.directiveName != QLatin1String( "term" ) )
    {
        return;
    }

    const QList< CompletionDisplayItem > local = localCandidatesFor( shownContext_ );
    if( local.isEmpty() )
        return;

    offlineItems_ = local;
    popup_->setItems( offlineItems_ );
    popup_->updateFilter( shownContext_.prefix );
    showOrRefreshPopup();
}

void CompletionCoordinator::showHoverDetail( const QString& role, const QString& target,
                                             const QPoint& globalPos )
{
    if( detail_.isNull() || glossary_ == nullptr )
        return;
    // 자동완성이 떠 있으면 그쪽이 우선이다. 두 팝업이 겹치면 읽을 수 없다.
    if( isPopupVisible() )
        return;
    if( role != QLatin1String( "term" ) || target.trimmed().isEmpty() )
    {
        detail_->hide();
        return;
    }

    const GlossaryEntry* entry = glossary_->lookup( target );
    if( entry == nullptr )
    {
        detail_->hide();
        return;
    }

    if( !activeView_.isNull() )
    {
        QWidget* window = activeView_->window();
        if( detail_->parentWidget() != window )
            detail_->setParent( window, detail_->windowFlags() );
    }

    const QString source = entry->path.isEmpty()
                               ? QString{}
                               : QStringLiteral( "%1:%2" )
                                     .arg( QFileInfo( entry->path ).fileName() )
                                     .arg( entry->line );
    if( !detail_->setContent( entry->term, entry->definition, source ) )
    {
        detail_->hide();
        return;
    }

    detail_->showNearPoint( globalPos );
}

void CompletionCoordinator::hideHoverDetail()
{
    if( !detail_.isNull() && !isPopupVisible() )
        detail_->hide();
}

void CompletionCoordinator::attachEditor( QTextView* view )
{
    if( view == nullptr )
        return;

    connect( view, &QTextView::sigCharAdded, this, &CompletionCoordinator::onCharAdded,
            Qt::UniqueConnection );
    // 캐럿이 다른 곳으로 뛰면 지금 띄운 후보는 무의미하다.
    connect( view, &QTextView::sigViewportScrolled, this, &CompletionCoordinator::hidePopup,
            Qt::UniqueConnection );
}

void CompletionCoordinator::detachEditor( QTextView* view )
{
    if( view == nullptr )
        return;

    disconnect( view, nullptr, this, nullptr );
    if( activeView_ == view )
    {
        hidePopup();
        activeView_ = nullptr;
    }
}

void CompletionCoordinator::setActiveEditor( QTextView* view )
{
    if( activeView_ == view )
        return;

    hidePopup();
    activeView_ = view;
}

void CompletionCoordinator::setActiveProjectId( const QString& projectId )
{
    if( activeProjectId_ == projectId )
        return;

    activeProjectId_ = projectId;
    hidePopup();
}

bool CompletionCoordinator::isPopupVisible() const
{
    return popup_ != nullptr && popup_->isActive();
}

void CompletionCoordinator::hidePopup()
{
    // popup_->hide() 는 popupHidden 을 거쳐 이 함수로 되돌아온다. 한 번만 돈다.
    if( hidingPopup_ )
        return;
    const QScopedValueRollback< bool > guard( hidingPopup_, true );

    debounce_->stop();
    pendingTrigger_.clear();
    pendingExplicit_ = false;
    inFlight_ = {};

    if( !detail_.isNull() && detail_->isVisible() )
        detail_->hide();

    if( popup_ != nullptr && popup_->isVisible() )
        popup_->hide();

    // 팝업이 없는 동안 모든 키 이벤트를 훑을 이유가 없다.
    qApp->removeEventFilter( this );
}

void CompletionCoordinator::requestExplicit()
{
    debounce_->stop();
    trigger( QString{}, true );
}

// ── 트리거 ────────────────────────────────────────────────

void CompletionCoordinator::onCharAdded( const int character )
{
    if( activeView_.isNull() || sender() != activeView_ )
        return;

    if( isPopupVisible() )
    {
        // 이미 떠 있으면 다시 묻지 않고 필터만 좁힌다. 매 글자 LSP 왕복은 낭비다.
        const rstcomplete::Context context = contextAtCaret();
        if( context.kind == rstcomplete::ContextKind::None )
        {
            hidePopup();
            return;
        }
        shownContext_ = context;
        popup_->updateFilter( context.prefix );
        return;
    }

    if( !isTriggerCharacter( character ) )
        return;

    pendingTrigger_ = QString( QChar( character ) );
    pendingExplicit_ = false;
    debounce_->start();
}

void CompletionCoordinator::flushTrigger()
{
    const QString triggerCharacter = pendingTrigger_;
    const bool explicitInvoke = pendingExplicit_;
    pendingTrigger_.clear();
    pendingExplicit_ = false;

    trigger( triggerCharacter, explicitInvoke );
}

void CompletionCoordinator::trigger( const QString& triggerCharacter, const bool explicitInvoke )
{
    if( activeView_.isNull() || popup_.isNull() )
        return;

    const rstcomplete::Context context = contextAtCaret();
    if( context.kind == rstcomplete::ContextKind::None )
    {
        // Ctrl+Space 를 눌렀는데 완성할 것이 없으면 조용히 넘어간다.
        hidePopup();
        return;
    }

    shownContext_ = context;

    // 접두를 뗀 전체 후보를 팝업에 주고 걸러내기는 팝업의 퍼지 매칭에 맡긴다.
    // 그래야 "cb" 로 "code-block" 을 고를 수 있다.
    rstcomplete::Context broad = context;
    broad.prefix.clear();
    const QVector< rstcomplete::Item > offline =
        rstcomplete::finalizeItems( rstcomplete::candidatesFor( broad ) );
    offlineItems_ = toDisplayList( offline );
    // 오프라인 표가 비워 두는 롤 대상(:term: 등)은 워크스페이스 인덱스가 채운다.
    offlineItems_ += localCandidatesFor( context );

    if( !offlineItems_.isEmpty() )
    {
        popup_->setItems( offlineItems_ );
        popup_->updateFilter( context.prefix );
        showPopupAtCaret();
    }
    else if( !explicitInvoke )
    {
        // 오프라인 후보가 없는 컨텍스트(:ref: 대상, 경로)는 LSP 응답을 기다린다.
        popup_->hide();
    }

    askLsp( triggerCharacter );
}

void CompletionCoordinator::askLsp( const QString& triggerCharacter )
{
    const QString path = editorPath();
    if( activeProjectId_.isEmpty() || path.isEmpty() || activeView_.isNull() )
        return;

    inFlight_ = {};
    inFlight_.projectId = activeProjectId_;
    inFlight_.path = path;
    inFlight_.line = activeView_->caretLine();
    inFlight_.column = activeView_->caretColumn();
    inFlight_.triggerCharacter = isEsbonioTriggerCharacter( triggerCharacter ) ? triggerCharacter
                                                                              : QString{};

    // 받는 쪽이 registerRequestId() 로 id 를 돌려준다 (직접 연결이라 동기적이다).
    emit lspCompletionRequested( inFlight_.path, inFlight_.line, inFlight_.column,
                                inFlight_.triggerCharacter );
}

void CompletionCoordinator::registerRequestId( const int requestId )
{
    if( requestId <= 0 )
    {
        inFlight_ = {};
        return;
    }
    inFlight_.requestId = requestId;
}

// ── LSP 응답 ──────────────────────────────────────────────

void CompletionCoordinator::applyLspItems( const QString& projectId, const int requestId,
                                           const QList< LspCompletionItem >& items )
{
    if( requestId <= 0 || requestId != inFlight_.requestId || projectId != inFlight_.projectId )
        return;   // 이미 지나간 요청의 응답
    if( popup_.isNull() )
        return;

    if( items.isEmpty() )
    {
        // Esbonio 는 내부 Sphinx 빌드 전에는 후보를 만들지 못한다. 빌드가
        // 끝나면 한 번만 다시 묻는다. 오프라인 팝업은 그대로 둔다.
        if( !warmProjects_.contains( projectId ) && !inFlight_.retried )
        {
            retryAfterBuild_.insert( projectId );
            emit logMessage( tr( "LSP 완성 결과가 비어 있습니다 [%1]. 빌드 후 다시 시도합니다." )
                                .arg( projectId ) );
        }
        return;
    }

    warmProjects_.insert( projectId );
    retryAfterBuild_.remove( projectId );

    QVector< rstcomplete::Item > converted;
    converted.reserve( items.size() );
    for( const LspCompletionItem& item : items )
        converted.push_back( fromLsp( item ) );

    harvestVocabulary( items );

    const QString lineText = activeView_.isNull() ? QString{}
                                                  : activeView_->lineText( inFlight_.line );
    converted = rstcomplete::normalizeLspItems( std::move( converted ), lineText, inFlight_.column );

    // LSP 가 우선, 오프라인 표는 빈 자리만 메운다.
    QVector< rstcomplete::Item > offline;
    offline.reserve( offlineItems_.size() );
    for( const CompletionDisplayItem& item : offlineItems_ )
        offline.push_back( { item.label, item.insertText, item.detail, item.kind } );

    const QVector< rstcomplete::Item > merged =
        rstcomplete::finalizeItems( rstcomplete::mergeItems( std::move( converted ), offline ) );

    popup_->setItems( toDisplayList( merged ) );
    popup_->updateFilter( shownContext_.prefix );
    showOrRefreshPopup();
}

void CompletionCoordinator::notifyBuildComplete( const QString& projectId )
{
    if( !retryAfterBuild_.remove( projectId ) )
        return;
    if( projectId != activeProjectId_ || !isPopupVisible() )
        return;   // 사용자가 이미 딴 데로 갔다. 갑자기 팝업을 띄우지 않는다.

    emit logMessage( tr( "빌드 완료 후 LSP 완성을 다시 요청합니다 [%1]." ).arg( projectId ) );
    QTimer::singleShot( 250, this,
                       [ this, projectId ]
                       {
                           if( projectId != activeProjectId_ || !isPopupVisible() )
                               return;
                           askLsp( QString{} );
                           inFlight_.retried = true;
                       } );
}

void CompletionCoordinator::harvestVocabulary( const QList< LspCompletionItem >& items )
{
    // 분류 규칙은 렉서의 캐시가 이미 갖고 있다. 여기서 다시 구현하면
    // 두 곳이 조용히 어긋난다. 스크래치 캐시에 먹여 결과만 꺼낸다.
    std::vector< rst::CompletionEntry > entries;
    entries.reserve( items.size() );
    for( const LspCompletionItem& item : items )
    {
        entries.push_back( { item.label.toStdString(), item.insertText.toStdString(),
                            item.detail.toStdString() } );
    }

    rst::RstMetadataCache scratch;
    scratch.updateFromCompletion( entries );
    if( !scratch.directivesPopulated && !scratch.rolesPopulated )
        return;

    QStringList directives;
    directives.reserve( static_cast< qsizetype >( scratch.directives.size() ) );
    for( const std::string& name : scratch.directives )
        directives << QString::fromStdString( name );

    QStringList roles;
    roles.reserve( static_cast< qsizetype >( scratch.roles.size() ) );
    for( const std::string& name : scratch.roles )
        roles << QString::fromStdString( name );

    emit vocabularyHarvested( directives, roles );
}

// ── 삽입 ──────────────────────────────────────────────────

void CompletionCoordinator::insertCompletion( const QString& insertText )
{
    if( activeView_.isNull() || insertText.isEmpty() )
        return;

    // 팝업을 띄운 뒤로 글자를 더 쳤을 수 있으므로 지울 길이를 지금 다시 잰다.
    const rstcomplete::Context now = contextAtCaret();
    const int replaceLength = now.kind == shownContext_.kind ? now.replaceLength
                                                             : shownContext_.replaceLength;

    activeView_->replaceRangeAtCursor( qMax( 0, replaceLength ), insertText );
    hidePopup();
}

// ── 컨텍스트 ──────────────────────────────────────────────

rstcomplete::Context CompletionCoordinator::contextAtCaret() const
{
    if( activeView_.isNull() )
        return {};

    const int line = activeView_->caretLine();
    return rstcomplete::detectContext( activeView_->lineText( line ), activeView_->caretColumn(),
                                      previousLinesAtCaret( 12 ) );
}

QStringList CompletionCoordinator::previousLinesAtCaret( const int count ) const
{
    if( activeView_.isNull() )
        return {};

    QStringList lines;
    const int current = activeView_->caretLine();
    for( int line = current - 1; line >= 1 && lines.size() < count; --line )
        lines << activeView_->lineText( line );
    return lines;   // 역순: 바로 앞 줄이 [0]
}

QString CompletionCoordinator::editorPath() const
{
    return activeView_.isNull() ? QString{} : activeView_->currentFilePath();
}

// ── 키 입력 ───────────────────────────────────────────────

bool CompletionCoordinator::eventFilter( QObject* watched, QEvent* event )
{
    if( !isPopupVisible() )
        return QObject::eventFilter( watched, event );

    if( event->type() == QEvent::KeyPress )
    {
        // 편집기(또는 그 안의 Scintilla 위젯)가 받은 키만 가로챈다.
        // 팝업은 포커스를 갖지 않으므로 키는 언제나 편집기로 간다.
        auto* widget = qobject_cast< QWidget* >( watched );
        const bool insideEditor = widget != nullptr && !activeView_.isNull()
                               && ( widget == activeView_ || activeView_->isAncestorOf( widget ) );
        if( insideEditor && popup_->handleKeyPress( static_cast< QKeyEvent* >( event ) ) )
            return true;
    }
    else if( event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate )
    {
        auto* widget = qobject_cast< QWidget* >( watched );
        if( widget != nullptr && !activeView_.isNull()
            && ( widget == activeView_ || activeView_->isAncestorOf( widget ) ) )
        {
            hidePopup();
        }
    }
    else if( event->type() == QEvent::Move || event->type() == QEvent::Resize )
    {
        // 창이 움직이면 팝업만 제자리에 남아 떠다닌다.
        if( !activeView_.isNull() && watched == activeView_->window() )
            hidePopup();
    }

    return QObject::eventFilter( watched, event );
}

}  // namespace mrst

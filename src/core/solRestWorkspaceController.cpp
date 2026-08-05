#include "stdafx.h"
#include "solRestWorkspaceController.hpp"

#include "solAppSettings.hpp"
#include "solPythonEnvMgr.hpp"
#include "solSphinxPreviewController.hpp"
#include "solSphinxProjectRegistry.hpp"
#include "editor/QBaseEditor.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QWebEngineView>

namespace mrst {

namespace {

/// 프로젝트를 못 찾았을 때 쓰는 표시용 이름.
QString unresolvedProjectLabel()
{
    return WorkspaceController::tr( "(프로젝트 없음)" );
}

}  // namespace

WorkspaceController::WorkspaceController( QObject* parent )
    : QObject( parent )
    , registry_( new ProjectRegistry( this ) )
    , previewController_( new SphinxPreviewController( this ) )
{
    connect( previewController_, &SphinxPreviewController::logMessage, this, &WorkspaceController::logMessage );
    connect( previewController_, &SphinxPreviewController::diagnosticsReady, this,
            [this]( const QString& source, const QVector< DiagnosticEntry >& entries ) {
                emit diagnosticsChanged( source, entries );
            } );
    connect( previewController_, &SphinxPreviewController::missingDependenciesDetected, this,
            &WorkspaceController::missingDependenciesDetected );
    connect( previewController_, &SphinxPreviewController::buildFinished, this,
            &WorkspaceController::onPreviewFinished );

    connect( registry_, &ProjectRegistry::logMessage, this, &WorkspaceController::logMessage );
    connect( registry_, &ProjectRegistry::scanStarted, this, [this] {
        emit logMessage( tr( "Sphinx 프로젝트를 검색하는 중..." ) );
    } );
    connect( registry_, &ProjectRegistry::scanFinished, this, [this]( int count ) {
        logProjectList();
        emit projectsChanged( count );

        // 스캔 완료 시점에 이미 열려 있던 문서들의 프로젝트를 뒤늦게 확정한다.
        for( DocumentContext& context : documents_ )
        {
            if( context.projectId.isEmpty() )
                resolveProject( context );
        }
        if( activeView_ )
            setActiveDocument( activeView_ );
    } );

    reloadSettings();
}

WorkspaceController::~WorkspaceController() = default;

void WorkspaceController::setPreviewView( QWebEngineView* view )
{
    previewView_ = view;
    if( previewView_ == nullptr )
        return;

    connect( previewView_, &QWebEngineView::loadFinished, this, [this]( const bool ok ) {
        if( !ok )
            emit logMessage( tr( "프리뷰 HTML 로드 실패" ) );
        else
            emit logMessage( tr( "프리뷰 표시: %1" ).arg( previewView_->url().fileName() ) );
    } );
}

void WorkspaceController::setPythonEnvironment( PythonEnvManager* manager )
{
    pythonEnv_ = manager;
    if( pythonEnv_ != nullptr && previewController_ != nullptr )
        previewController_->setShadowDir( pythonEnv_->shadowDir() );
}

QString WorkspaceController::writeShadowCopy( QTextView* view, const QString& path ) const
{
    if( view == nullptr || pythonEnv_ == nullptr || !view->isModified() )
        return {};

    const QString directory = pythonEnv_->shadowDir();
    if( directory.isEmpty() || !QDir().mkpath( directory ) )
        return {};

    // 원본 경로 해시로 이름을 만들어 문서마다 파일 하나를 재사용한다.
    const QByteArray digest = QCryptographicHash::hash( path.toUtf8(), QCryptographicHash::Sha1 );
    const QString shadowPath = QDir( directory ).filePath(
        QStringLiteral( "%1.%2" ).arg( QString::fromLatin1( digest.toHex().left( 16 ) ),
                                      QFileInfo( path ).suffix() ) );

    QFile file( shadowPath );
    if( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
        return {};

    file.write( view->text().toUtf8() );
    file.close();
    return shadowPath;
}

void WorkspaceController::requestPreviewBuild( const bool immediate )
{
    if( shuttingDown_ || previewController_ == nullptr || pythonEnv_ == nullptr )
        return;

    // 런타임이 준비되기 전에는 조용히 넘어간다. 준비되면 다시 호출된다.
    if( !pythonEnv_->isReady() )
        return;

    QTextView* view = activeView_;
    DocumentContext* context = contextFor( view );
    if( view == nullptr || context == nullptr || context->path.isEmpty() )
        return;

    const SphinxProject* project = registry_->findById( context->projectId );
    if( project == nullptr )
        return;   // 가상 프로젝트는 Phase 7 에서 처리한다.

    PreviewBuildRequest request;
    request.project = *project;
    request.pythonExe = pythonEnv_->pythonExe();
    request.builderScript = pythonEnv_->previewBuilderScript();
    request.sourceFile = context->path;
    request.shadowFile = writeShadowCopy( view, context->path );

    if( immediate )
        previewController_->buildNow( request );
    else
        previewController_->requestBuild( request );
}

void WorkspaceController::onPreviewFinished( const PreviewBuildResult& result )
{
    if( result.cancelled || previewView_ == nullptr )
        return;

    if( !result.ok || result.htmlPath.isEmpty() )
        return;

    previewView_->load( QUrl::fromLocalFile( result.htmlPath ) );
}

ProjectRegistry* WorkspaceController::projectRegistry() const
{
    return registry_;
}

QString WorkspaceController::workspaceRoot() const
{
    return registry_->workspaceRoot();
}

QString WorkspaceController::activeProjectId() const
{
    return activeProjectId_;
}

void WorkspaceController::setWorkspaceRoot( const QString& root )
{
    registry_->setWorkspaceRoot( root );
    if( registry_->workspaceRoot().isEmpty() )
        return;

    // 캐시가 있으면 즉시 사용 가능한 목록을 얻고, 그와 별개로 항상 재스캔한다.
    // (캐시는 첫 화면을 빠르게 만들 뿐 진실의 원천이 아니다.)
    if( registry_->loadCache() )
    {
        logProjectList();
        emit projectsChanged( static_cast< int >( registry_->projects().size() ) );
    }
    registry_->rescanAsync();
}

void WorkspaceController::rescanProjects()
{
    registry_->rescanAsync();
}

void WorkspaceController::reloadSettings()
{
    AppSettings settings;
    ScannerSettings scanner;
    scanner.buildDirName = settings.value( QStringLiteral( "esbonio/buildDirName" ),
                                          QStringLiteral( "_build/multiroot-rest" ) )
                              .toString()
                              .toStdString();
    registry_->setScannerSettings( std::move( scanner ) );
}

void WorkspaceController::shutdown()
{
    shuttingDown_ = true;
    if( previewController_ != nullptr )
        previewController_->cancel();
    documents_.clear();
    activeView_ = nullptr;
    activeProjectId_.clear();
}

void WorkspaceController::attachDocument( QTextView* view )
{
    if( view == nullptr || shuttingDown_ || documents_.contains( view ) )
        return;

    DocumentContext context;
    context.view = view;
    context.path = view->currentFilePath().isEmpty() ? QString{} : QFileInfo( view->currentFilePath() ).absoluteFilePath();
    resolveProject( context );
    documents_.insert( view, context );

    // 편집 중에는 디바운스된 프리뷰 재빌드만 건다.
    connect( view, &QTextView::sigTextEdited, this, [this, view] {
        if( activeView_ == view )
            requestPreviewBuild( false );
    } );
}

void WorkspaceController::detachDocument( QTextView* view )
{
    if( view == nullptr )
        return;

    documents_.remove( view );
    if( activeView_ == view )
    {
        activeView_ = nullptr;
        activeProjectId_.clear();
    }
}

void WorkspaceController::setActiveDocument( QTextView* view )
{
    if( shuttingDown_ )
        return;

    activeView_ = view;
    if( view == nullptr )
    {
        activeProjectId_.clear();
        return;
    }

    DocumentContext* context = contextFor( view );
    if( context == nullptr )
    {
        attachDocument( view );
        context = contextFor( view );
        if( context == nullptr )
            return;
    }

    // 파일이 저장/이름변경으로 바뀌었을 수 있으므로 경로를 다시 확인한다.
    const QString currentPath = view->currentFilePath().isEmpty() ? QString{} : QFileInfo( view->currentFilePath() ).absoluteFilePath();
    if( currentPath != context->path )
    {
        context->path = currentPath;
        context->projectId.clear();
        resolveProject( *context );
    }
    else if( context->projectId.isEmpty() )
    {
        resolveProject( *context );
    }

    if( context->projectId == activeProjectId_ )
        return;

    activeProjectId_ = context->projectId;
    emit activeProjectChanged( activeProjectId_, context->isVirtual );
    emit logMessage( tr( "활성 프로젝트: %1" )
                        .arg( activeProjectId_.isEmpty() ? unresolvedProjectLabel() : activeProjectId_ ) );

    requestPreviewBuild( true );
}

void WorkspaceController::notifyDocumentSaved( QTextView* view )
{
    if( view == nullptr || shuttingDown_ )
        return;

    // 저장으로 경로가 확정되었을 수 있으니 프로젝트를 다시 해석한다.
    setActiveDocument( view );
}

DocumentContext* WorkspaceController::contextFor( QTextView* view )
{
    const auto it = documents_.find( view );
    return it == documents_.end() ? nullptr : &it.value();
}

void WorkspaceController::resolveProject( DocumentContext& context )
{
    context.projectId.clear();
    context.isVirtual = false;
    if( context.path.isEmpty() )
        return;

    if( const SphinxProject* project = registry_->resolveForFile( context.path ) )
        context.projectId = QString::fromStdWString( project->projectId );
}

void WorkspaceController::logProjectList()
{
    const std::vector< SphinxProject >& projects = registry_->projects();
    emit logMessage( tr( "Sphinx 프로젝트 %1개 발견" ).arg( projects.size() ) );
    for( std::size_t idx = 0; idx < projects.size(); ++idx )
    {
        emit logMessage( QStringLiteral( "[%1]  %2 — %3" )
                            .arg( idx + 1 )
                            .arg( QString::fromStdWString( projects[ idx ].projectId ) )
                            .arg( toQString( projects[ idx ].rootPath ) ) );
    }
}

}  // namespace mrst

#include "stdafx.h"
#include "solPreviewBridge.hpp"

#include <QFile>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>

namespace mrst {
namespace {

QString readResourceText( const QString& path )
{
    QFile file( path );
    if( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
        return {};
    return QString::fromUtf8( file.readAll() );
}

}  // namespace

PreviewBridge::PreviewBridge( QObject* parent )
    : QObject( parent )
{
}

void PreviewBridge::attachTo( QWebEngineView* view )
{
    if( view == nullptr || view->page() == nullptr )
        return;

    auto* channel = new QWebChannel( this );
    channel->registerObject( QStringLiteral( "bridge" ), this );
    view->page()->setWebChannel( channel );

    // <script src="qrc:///..."> 로 넣으면 file:// 출처 제약에 걸린다.
    // qwebchannel.js 를 C++ 에서 읽어 스크립트로 주입하면 모든 내비게이션에
    // 자동 재적용되고 출처 문제도 없다.
    const QString transport = readResourceText( QStringLiteral( ":/qtwebchannel/qwebchannel.js" ) );
    const QString previewScript = readResourceText( QStringLiteral( ":/preview/mrr_preview.js" ) );

    QWebEngineScriptCollection& scripts = view->page()->scripts();

    if( !transport.isEmpty() )
    {
        QWebEngineScript channelScript;
        channelScript.setName( QStringLiteral( "mrr_qwebchannel" ) );
        channelScript.setInjectionPoint( QWebEngineScript::DocumentCreation );
        channelScript.setWorldId( QWebEngineScript::MainWorld );
        channelScript.setRunsOnSubFrames( false );
        channelScript.setSourceCode( transport );
        scripts.insert( channelScript );
    }

    if( !previewScript.isEmpty() )
    {
        QWebEngineScript pageScript;
        pageScript.setName( QStringLiteral( "mrr_preview" ) );
        pageScript.setInjectionPoint( QWebEngineScript::DocumentReady );
        pageScript.setWorldId( QWebEngineScript::MainWorld );
        pageScript.setRunsOnSubFrames( false );
        pageScript.setSourceCode( previewScript );
        scripts.insert( pageScript );
    }
}

bool PreviewBridge::isReady() const
{
    return ready_;
}

void PreviewBridge::resetReady()
{
    ready_ = false;
}

void PreviewBridge::requestScrollToLine( const int sourceIndex, const double line, const double ratio )
{
    if( !ready_ )
    {
        // 페이지가 아직 준비되지 않았다. 마지막 요청 하나만 들고 있다가
        // 핸드셰이크 직후에 보낸다.
        hasPendingScroll_ = true;
        pendingSourceIndex_ = sourceIndex;
        pendingLine_ = line;
        pendingRatio_ = ratio;
        return;
    }

    emit scrollToLineRequested( sourceIndex, line, ratio );
}

void PreviewBridge::requestRebind()
{
    if( ready_ )
        emit rebindRequested();
}

void PreviewBridge::suppressScrollFeedback( const int milliseconds )
{
    if( ready_ )
        emit scrollFeedbackSuppressed( milliseconds );
}

void PreviewBridge::ready( int /*protocolVersion*/ )
{
    ready_ = true;
    emit bridgeReady();

    if( hasPendingScroll_ )
    {
        hasPendingScroll_ = false;
        emit scrollToLineRequested( pendingSourceIndex_, pendingLine_, pendingRatio_ );
    }
}

void PreviewBridge::sourceLocationClicked( const int sourceIndex, const double line, const double viewportRatio )
{
    emit editorNavigationRequested( sourceIndex, line, viewportRatio );
}

void PreviewBridge::previewScrolled( const int sourceIndex, const double line, const double viewportRatio,
                                     const bool userDriven )
{
    emit previewScrollChanged( sourceIndex, line, viewportRatio, userDriven );
}

void PreviewBridge::requestHotSwap( const QString& documentHtml, const QString& baseUrl, const int token )
{
    if( !ready_ )
        return;

    emit hotSwapRequested( documentHtml, baseUrl, token );
}

void PreviewBridge::hotSwapResult( const int token, const bool ok, const QString& message )
{
    emit hotSwapCompleted( token, ok, message );
}

void PreviewBridge::requestMarkdownRender( const QString& text, const QString& baseUrl,
                                           const QString& optionsJson, const int token )
{
    if( !ready_ )
        return;

    emit markdownSourceChanged( text, baseUrl, optionsJson, token );
}

void PreviewBridge::markdownRendered( const int token, const bool ok, const QString& message )
{
    emit markdownRenderCompleted( token, ok, message );
}

void PreviewBridge::markdownRendererReady( const QString& origin, const QString& version )
{
    emit markdownRendererOrigin( origin, version );
}

void PreviewBridge::markdownAssetFailed( const QString& assetId, const QString& reason )
{
    emit markdownAssetLoadFailed( assetId, reason );
}

}  // namespace mrst

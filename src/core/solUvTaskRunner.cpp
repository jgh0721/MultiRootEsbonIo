#include "stdafx.h"
#include "solUvTaskRunner.hpp"

#include <QRegularExpression>
#include <QTimer>

namespace mrst {
namespace {

/// terminate() 이후 kill() 까지 기다리는 시간.
constexpr int kKillGraceMs = 800;

}  // namespace

QString stripAnsiEscapes( const QString& text )
{
    // CSI 시퀀스( ESC [ ... 종결문자 )와 OSC 시퀀스( ESC ] ... BEL )를 제거한다.
    static const QRegularExpression csi( QStringLiteral( "\x1B\\[[0-9;?]*[ -/]*[@-~]" ) );
    static const QRegularExpression osc( QStringLiteral( "\x1B\\][^\x07\x1B]*(?:\x07|\x1B\\\\)" ) );

    QString cleaned = text;
    cleaned.remove( csi );
    cleaned.remove( osc );
    return cleaned;
}

QProcessEnvironment utf8ProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert( QStringLiteral( "PYTHONUTF8" ), QStringLiteral( "1" ) );
    env.insert( QStringLiteral( "PYTHONIOENCODING" ), QStringLiteral( "utf-8" ) );
    // 색상 자체를 끄면 ANSI 제거 부담이 줄어든다.
    env.insert( QStringLiteral( "NO_COLOR" ), QStringLiteral( "1" ) );
    return env;
}

UvTask::UvTask( Request request, QObject* parent )
    : QObject( parent )
    , request_( std::move( request ) )
{
    if( request_.environment.isEmpty() )
        request_.environment = utf8ProcessEnvironment();
}

UvTask::~UvTask()
{
    if( process_ && process_->state() != QProcess::NotRunning )
    {
        process_->kill();
        process_->waitForFinished( 200 );   // 소멸 시점의 마지막 수단
    }
}

QString UvTask::tag() const
{
    return request_.tag;
}

QString UvTask::collectedOutput() const
{
    return collected_;
}

bool UvTask::isRunning() const
{
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

bool UvTask::wasCancelled() const
{
    return cancelled_;
}

void UvTask::start()
{
    if( process_ != nullptr )
        return;

    process_ = std::make_unique< QProcess >();
    process_->setProgram( request_.program );
    process_->setArguments( request_.arguments );
    if( !request_.workingDirectory.isEmpty() )
        process_->setWorkingDirectory( request_.workingDirectory );
    process_->setProcessEnvironment( request_.environment );
    process_->setProcessChannelMode( request_.mergeChannels ? QProcess::MergedChannels
                                                            : QProcess::SeparateChannels );

    connect( process_.get(), &QProcess::readyReadStandardOutput, this, [this] { drain( false ); } );
    if( !request_.mergeChannels )
        connect( process_.get(), &QProcess::readyReadStandardError, this, [this] { drain( false ); } );
    connect( process_.get(), &QProcess::finished, this, &UvTask::onFinished );
    connect( process_.get(), &QProcess::errorOccurred, this, &UvTask::onErrorOccurred );
    connect( process_.get(), &QProcess::started, this, &UvTask::started );

    process_->start();
}

void UvTask::cancel()
{
    if( cancelled_ || !isRunning() )
        return;

    cancelled_ = true;
    process_->terminate();

    QPointer< UvTask > guard( this );
    QTimer::singleShot( kKillGraceMs, this, [guard] {
        if( guard && guard->isRunning() )
            guard->process_->kill();
    } );
}

void UvTask::drain( bool flushPartial )
{
    if( process_ == nullptr )
        return;

    pending_ += QString::fromUtf8( process_->readAllStandardOutput() );
    if( !request_.mergeChannels )
        pending_ += QString::fromUtf8( process_->readAllStandardError() );

    // uv 는 진행률을 CR 로 덮어쓰므로 CR 도 줄 구분자로 취급한다.
    pending_.replace( QLatin1Char( '\r' ), QLatin1Char( '\n' ) );

    qsizetype newline = pending_.indexOf( QLatin1Char( '\n' ) );
    while( newline >= 0 )
    {
        const QString line = stripAnsiEscapes( pending_.left( newline ) ).trimmed();
        pending_.remove( 0, newline + 1 );
        if( !line.isEmpty() )
        {
            collected_ += line;
            collected_ += QLatin1Char( '\n' );
            emit outputLine( line );
        }
        newline = pending_.indexOf( QLatin1Char( '\n' ) );
    }

    if( flushPartial && !pending_.isEmpty() )
    {
        const QString line = stripAnsiEscapes( pending_ ).trimmed();
        pending_.clear();
        if( !line.isEmpty() )
        {
            collected_ += line;
            collected_ += QLatin1Char( '\n' );
            emit outputLine( line );
        }
    }
}

void UvTask::onFinished( int exitCode, QProcess::ExitStatus status )
{
    if( finishedEmitted_ )
        return;

    finishedEmitted_ = true;
    drain( true );
    emit finished( exitCode, status == QProcess::CrashExit );
}

void UvTask::onErrorOccurred( QProcess::ProcessError error )
{
    if( error != QProcess::FailedToStart )
        return;   // 그 외 오류는 finished 로 이어진다.

    if( finishedEmitted_ )
        return;

    finishedEmitted_ = true;
    emit failedToStart( tr( "실행할 수 없습니다: %1" ).arg( QDir::toNativeSeparators( request_.program ) ) );
}

}  // namespace mrst

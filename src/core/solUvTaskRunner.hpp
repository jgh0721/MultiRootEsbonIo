#pragma once

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <memory>

namespace mrst {

/// uv / python 자식 프로세스 하나를 비동기로 실행한다.
///
/// GUI 스레드 전용이며 waitFor*() 나 processEvents() 를 절대 쓰지 않는다.
/// (기존 PythonEnvManager 는 uv sync 가 도는 수 분 동안 processEvents 를
/// 돌려서, 타이머와 QProcess 콜백이 그 사이에 재진입할 수 있었다.)
class UvTask final : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString                         program;              ///< 절대 경로
        QStringList                     arguments;
        QString                         workingDirectory;
        QProcessEnvironment             environment;          ///< 비면 utf8ProcessEnvironment()
        bool                            mergeChannels = true;
        int                             startTimeoutMs = 15000;
        QString                         tag;                  ///< 로그 표시용 (예: "uv sync")
    };

    explicit UvTask( Request request, QObject* parent = nullptr );
    ~UvTask() override;

    void                                start();
    /// 비차단 취소. terminate() 후에도 안 죽으면 잠시 뒤 kill() 한다.
    /// (Windows 에서 terminate() 는 콘솔 없는 python 이 무시하는 WM_CLOSE 다.)
    void                                cancel();

    [[nodiscard]] bool                  isRunning() const;
    [[nodiscard]] bool                  wasCancelled() const;
    [[nodiscard]] QString               tag() const;
    [[nodiscard]] QString               collectedOutput() const;

signals:
    void                                started();
    void                                outputLine( const QString& line );   ///< ANSI/CR 정리 완료
    void                                finished( int exitCode, bool crashed );
    void                                failedToStart( const QString& message );

private:
    void                                drain( bool flushPartial );
    void                                onFinished( int exitCode, QProcess::ExitStatus status );
    void                                onErrorOccurred( QProcess::ProcessError error );

    Request                             request_;
    std::unique_ptr< QProcess >         process_;
    QString                             pending_;             ///< 아직 개행을 못 만난 잔여 출력
    QString                             collected_;
    bool                                cancelled_ = false;
    bool                                finishedEmitted_ = false;
};

/// ANSI 이스케이프 시퀀스 제거. uv 는 NO_COLOR 로 대부분 억제되지만
/// 진행률 표시용 잔여 시퀀스가 남을 수 있다.
[[nodiscard]] QString                   stripAnsiEscapes( const QString& text );

/// 자식 프로세스가 UTF-8 로 말하도록 강제한 환경.
[[nodiscard]] QProcessEnvironment       utf8ProcessEnvironment();

}  // namespace mrst

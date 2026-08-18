#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

class QDialogButtonBox;
class QLabel;
class QPushButton;

namespace mrst {
class UpdateService;
}

/// 도움말 → 정보. 아이콘 / 현재 버전 / 최신 버전 / 저장소 주소만 보여 주는
/// 읽기 전용 대화상자다.
///
/// **UpdateService 를 소유하지 않는다.** QSettingsDialog 와 같은 규칙이다 —
/// 인스턴스가 둘이면 상태 머신이 어긋나므로, 상태를 읽는 것만 넘겨받은
/// 포인터로 하고 점검 요청은 시그널로 MainWindow 에 넘긴다.
class QAboutDialog final : public QDialog
{
    Q_OBJECT

public:
    /// updateService 는 nullptr 이어도 된다. 그때는 최신 버전 줄이 "확인하지
    /// 않음" 으로 남고 [지금 확인] 이 비활성된다.
    explicit QAboutDialog( mrst::UpdateService* Service, QWidget* Parent = nullptr );

signals:
    /// [지금 확인] 을 눌렀거나, 대화상자를 여는 순간 이번 실행에서 확인된 결과가
    /// 아직 없을 때. 받는 쪽이 자기 UpdateService 에 checkAsync(true) 를 시킨다.
    void                                updateCheckRequested();

public slots:
    /// 최신 버전 줄을 UpdateService 의 지금 상태로 다시 그린다.
    void                                refreshUpdateStatus();

protected:
    void                                showEvent( QShowEvent* Event ) override;
    /// QEvent::LanguageChange. 대화상자가 모달이라 열려 있는 동안 언어가 바뀔
    /// 일은 없지만, 규약을 깨 두면 나중에 비모달로 바뀔 때 조용히 틀린다.
    void                                changeEvent( QEvent* Event ) override;

private:
    /// 최신 버전 줄이 보여 줄 수 있는 상태.
    ///
    /// UpdateService::State 를 그대로 쓰지 않는 이유가 둘 있다. Failed 는
    /// 곧바로 Idle 로 되돌아가므로(fail() 이 setState 를 두 번 부른다) 실패를
    /// 여기서 따로 기억하지 않으면 화면에 남지 않는다. 그리고 "릴리스가 아직
    /// 없다(404)" 는 upToDate 로 오는데, 그것과 "이번 실행에서 확인하지 않았다"
    /// 는 둘 다 available() 이 비어 있어 상태만으로는 구분되지 않는다.
    enum class Latest
    {
        Unknown,        ///< 이번 실행에서 확인한 적이 없다
        Checking,
        UpToDate,
        NewVersion,
        NoRelease,      ///< 매니페스트가 없다(404). 정식 릴리스 이전
        Failed,
    };

    void                                buildUi();
    void                                connectUpdateService();
    void                                retranslateUi();
    void                                applyTitleBarTheme();
    /// 처음 보일 때 한 번. 자동 확인을 꺼 둔 사용자에게는 네트워크를 타지 않는다.
    void                                requestCheckIfIdle();
    [[nodiscard]] Latest                latestState() const;

    QPointer< mrst::UpdateService >     updateService_;    ///< 소유하지 않는다

    QLabel*                             iconLabel_ = nullptr;
    QLabel*                             productLabel_ = nullptr;
    QLabel*                             currentCaption_ = nullptr;
    QLabel*                             currentValue_ = nullptr;
    QLabel*                             latestCaption_ = nullptr;
    QLabel*                             latestValue_ = nullptr;
    QLabel*                             repositoryCaption_ = nullptr;
    QLabel*                             repositoryValue_ = nullptr;
    QPushButton*                        checkButton_ = nullptr;
    QDialogButtonBox*                   buttonBox_ = nullptr;

    /// 마지막 점검의 결과. 상태만으로는 복원되지 않아 여기 들고 있는다.
    bool                                checkFailed_ = false;
    bool                                noRelease_ = false;
    QString                             failureText_;
    bool                                firstShow_ = true;
};

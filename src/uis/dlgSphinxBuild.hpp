#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

/// 탐색기에서 프로젝트 루트를 오른쪽 클릭 → 빌드(&B)…
///
/// 빌더와 산출물 디렉터리를 고르고, 끝난 뒤 그 디렉터리를 탐색기로 열지를
/// 정한다. 실제 실행은 이 대화상자가 하지 않는다 — WorkspaceController 가
/// 프로젝트의 파이썬 환경을 알고 있고, 로그 패널도 그쪽 신호에 이미 물려 있다.
class QSphinxBuildDialog final : public QDialog
{
    Q_OBJECT

public:
    /// projectLabel 은 제목줄에 보일 이름, projectRoot 는 conf.py 가 있는 곳.
    QSphinxBuildDialog( const QString& projectLabel, const QString& projectRoot,
                        QWidget* parent = nullptr );

    [[nodiscard]] QString               builder() const;
    [[nodiscard]] QString               outputDirectory() const;
    [[nodiscard]] bool                  openWhenDone() const;

    /// 고른 값을 다음 번 기본값으로 남긴다. accept() 가 부른다.
    void                                rememberChoices() const;

protected:
    /// QEvent::LanguageChange. 모달이라 열려 있는 동안 언어가 바뀔 일은 없지만,
    /// 규약을 깨 두면 나중에 비모달로 바뀔 때 조용히 틀린다.
    void                                changeEvent( QEvent* event ) override;

private:
    void                                buildUi();
    void                                retranslateUi();
    /// 빌더가 바뀌면 산출물 경로도 따라간다. **사용자가 직접 고친 뒤로는
    /// 건드리지 않는다** — 애써 고른 경로를 콤보 조작이 되돌리면 안 된다.
    void                                onBuilderChanged();
    void                                onBrowseOutput();
    void                                updateOkState();

    QString                             projectLabel_;
    QString                             projectRoot_;
    /// 산출물 경로를 사람이 손댔는가.
    bool                                outputEdited_ = false;

    QLabel*                             builderLabel_ = nullptr;
    QComboBox*                          builderCombo_ = nullptr;
    QLabel*                             outputLabel_ = nullptr;
    QLineEdit*                          outputEdit_ = nullptr;
    QPushButton*                        browseButton_ = nullptr;
    QCheckBox*                          openCheck_ = nullptr;
    QLabel*                             hintLabel_ = nullptr;
    QDialogButtonBox*                   buttons_ = nullptr;
    QPushButton*                        buildButton_ = nullptr;
};

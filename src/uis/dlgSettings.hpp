#pragma once

#include "uniqueLibs/solDocument_Defs.hpp"
#include "ui_dlgSettings.h"

namespace mrst {
class PythonEnvManager;
}

class QSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit QSettingsDialog( QWidget* Parent = nullptr );

    static QList<ShortcutItem>          DefaultShortcuts();
    static QList<ShortcutItem>          LoadShortcutsFromSettings();
    static bool                         IsTextViewerRulerWidgetVisible();
    static void                         ApplyShortcutsToActions( const QList<ShortcutItem>& shortcuts, QWidget* topLevel );

signals:
    void                                settingsApplied();
    /// "지금 확인" 을 눌렀다. 이 대화상자는 UpdateService 를 소유하지 않는다 —
    /// 두 개가 돌면 상태 머신이 어긋나므로 MainWindow 가 가진 인스턴스에
    /// 요청만 넘긴다.
    void                                updateCheckRequested();

public slots:
    /// 마지막 확인 시각 라벨을 다시 읽는다. 대화상자가 열려 있는 동안
    /// UpdateService 의 상태가 바뀌면 MainWindow 가 불러 준다.
    void                                refreshUpdateStatus();

protected:
    void                                showEvent( QShowEvent* Event ) override;
    /// QEvent::LanguageChange 를 받아 페이지를 통째로 다시 만든다.
    void                                changeEvent( QEvent* Event ) override;

private slots:
    void                                on_btnOK_clicked( bool Checked = false );
    void                                on_btnCancel_clicked( bool Checked = false );
    void                                on_btnApply_clicked( bool Checked = false );

    void                                onResetShortcuts();

private:
    /// 좌측 카테고리와 5개 페이지를 (다시) 만든다.
    ///
    /// 언어를 바꾸면 통째로 다시 부른다. 이 파일에는 tr() 이 170곳 넘게 있고
    /// 거의 전부가 코드로 만든 위젯의 라벨이라, 별도의 retranslate 함수를 두면
    /// 문자열이 두 벌이 되어 한쪽만 고치는 사고가 반드시 난다.
    ///
    /// 그래서 **여기에는 페이지와 함께 죽는 연결만** 둔다. 대화상자만큼 오래
    /// 사는 객체(m_pythonEnvManager, Ui.lstCate)에 거는 연결은 생성자로 뺐다 —
    /// 여기 두면 언어를 바꿀 때마다 하나씩 더 붙는다.
    void                                buildPages();
    /// 대화상자 수명 동안 한 번만 걸어야 하는 연결. 전부 람다라
    /// Qt::UniqueConnection 을 쓸 수 없다(람다에 주면 fatal assert 다).
    void                                connectPythonEnvSignals();
    void                                applyTitleBarTheme();
    QWidget*                            createGeneralPage();
    QWidget*                            createShortcutsPage();
    QWidget*                            createEditorPage();
    QWidget*                            createPreviewPage();
    QWidget*                            createEsbonioPage();
    void                                loadShortcuts();
    void                                saveShortcuts();
    void                                loadTextViewerSettings();
    void                                saveTextViewerSettings();
    void                                loadPreviewSettings();
    void                                savePreviewSettings();
    void                                loadUpdateSettings();
    void                                loadEsbonioSettings();
    void                                saveEsbonioSettings();
    void                                refreshEsbonioStatus();
    void                                populateThemeColorTable();
    void                                populateThemeLexerCombo();
    void                                applyThemeScopeFilter();
    void                                applyThemePreview();
    QHash<QString, QColor>              collectThemeColors() const;
    void                                updateThemeColorButton( QPushButton* button, const QColor& color ) const;
    void                                updateThemeColorItem( QTableWidgetItem* item, const QColor& color ) const;

    Ui::dlgSettings                     Ui;

    /// 공통 페이지
    QComboBox*                          m_languageCombo = nullptr;
    QComboBox*                          m_themeCombo    = nullptr;
    QComboBox*                          m_themeScopeCombo = nullptr;
    QLabel*                             m_themeLexerListLabel = nullptr;
    QListWidget*                        m_themeLexerList = nullptr;
    QCheckBox*                          m_themeInstantPreviewCheck = nullptr;
    QLabel*                             m_themeNameLabel = nullptr;
    QTableWidget*                       m_themeColorTable = nullptr;
    QPushButton*                        m_themeImportButton = nullptr;
    QPushButton*                        m_themeExportButton = nullptr;
    QPushButton*                        m_themeResetButton = nullptr;
    QCheckBox*                          m_updateEnabledCheck = nullptr;
    QSpinBox*                           m_updateIntervalSpin = nullptr;
    QLabel*                             m_updateLastCheckedLabel = nullptr;
    QPushButton*                        m_updateCheckNowButton = nullptr;

    /// 단축키 페이지
    QList<ShortcutItem>                 m_shortcuts;
    QTableWidget*                       m_shortcutTable = nullptr;

    /// 편집기 페이지
    QFontComboBox*                      m_textFontCombo = nullptr;
    QSpinBox*                           m_textFontSizeSpin = nullptr;
    QComboBox*                          m_textFontRenderCombo = nullptr;
    QDoubleSpinBox*                     m_textLineSpacingSpin = nullptr;
    QFontComboBox*                      m_textRulerFontCombo = nullptr;
    QSpinBox*                           m_textRulerFontSizeSpin = nullptr;
    QSpinBox*                           m_textTabWidthSpin = nullptr;
    QCheckBox*                          m_textUseTabsCheck = nullptr;
    QCheckBox*                          m_textIndentGuidesCheck = nullptr;
    QComboBox*                          m_textIndentGuideStyleCombo = nullptr;
    QCheckBox*                          m_textWhitespaceCheck = nullptr;
    QComboBox*                          m_textWrapModeCombo = nullptr;
    QCheckBox*                          m_textWrapFlagEndCheck = nullptr;
    QCheckBox*                          m_textWrapFlagStartCheck = nullptr;
    QCheckBox*                          m_textWrapFlagMarginCheck = nullptr;
    QComboBox*                          m_textWrapIndentCombo = nullptr;
    QComboBox*                          m_textChangeHistoryCombo = nullptr;
    QCheckBox*                          m_textCodeFoldingCheck = nullptr;
    QCheckBox*                          m_textBraceHighlightCheck = nullptr;
    QComboBox*                          m_textSaveEncodingCombo = nullptr;
    QComboBox*                          m_textSaveBomCombo = nullptr;
    QCheckBox*                          m_textHotExitCheck = nullptr;
    QSpinBox*                           m_textLargeFileMBSpin = nullptr;
    QComboBox*                          m_textExternalChangeActionCombo = nullptr;
    QComboBox*                          m_textExternalChangeDetectionCombo = nullptr;
    QSpinBox*                           m_textExternalChangePollSpin = nullptr;

    /// 프리뷰 페이지
    QCheckBox*                          m_previewAllowRemoteCheck = nullptr;
    QCheckBox*                          m_previewUnsavedCheck = nullptr;
    QSpinBox*                           m_previewUnsavedMaxReadSpin = nullptr;
    QComboBox*                          m_previewVirtualThemeCombo = nullptr;
    QCheckBox*                          m_previewStubDoxygenCheck = nullptr;
    QSpinBox*                           m_previewOutlineDepthSpin = nullptr;
    class QComboBox*                    m_previewMathRendererCombo = nullptr;

    /// Python/Esbonio 페이지
    mrst::PythonEnvManager*             m_pythonEnvManager = nullptr;
    QCheckBox*                          m_useExternalUvCheck = nullptr;
    QLineEdit*                          m_uvPathEdit = nullptr;
    QPushButton*                        m_uvBrowseButton = nullptr;
    QLabel*                             m_detectedUvLabel = nullptr;
    QLabel*                             m_environmentRootLabel = nullptr;
    QLabel*                             m_configuredDateLabel = nullptr;
    QLabel*                             m_pythonExeLabel = nullptr;
    QLabel*                             m_sphinxBuildExeLabel = nullptr;
    QLabel*                             m_esbonioExeLabel = nullptr;
    class QSpinBox*                     m_maxLspProcessesSpin = nullptr;
    QPushButton*                        m_configurePythonButton = nullptr;
    QPushButton*                        m_cancelPythonButton = nullptr;
    class QProgressBar*                 m_pythonEnvProgress = nullptr;
    QTextEdit*                          m_pythonEnvLog = nullptr;
};
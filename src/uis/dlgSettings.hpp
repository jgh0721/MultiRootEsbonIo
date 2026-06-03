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

private slots:
    void                                on_btnOK_clicked( bool Checked = false );
    void                                on_btnCancel_clicked( bool Checked = false );
    void                                on_btnApply_clicked( bool Checked = false );

    void                                onResetShortcuts();

private:
    void                                setupUi();
    QWidget*                            createGeneralPage();
    QWidget*                            createShortcutsPage();
    QWidget*                            createEditorPage();
    QWidget*                            createEsbonioPage();
    void                                loadShortcuts();
    void                                saveShortcuts();
    void                                loadTextViewerSettings();
    void                                saveTextViewerSettings();
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
    QComboBox*                          m_textChangeHistoryCombo = nullptr;
    QCheckBox*                          m_textCodeFoldingCheck = nullptr;
    QCheckBox*                          m_textBraceHighlightCheck = nullptr;
    QComboBox*                          m_textSaveEncodingCombo = nullptr;
    QComboBox*                          m_textSaveBomCombo = nullptr;
    QCheckBox*                          m_textHotExitCheck = nullptr;
    QSpinBox*                           m_textLargeFileMBSpin = nullptr;

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
    QPushButton*                        m_configurePythonButton = nullptr;
    QTextEdit*                          m_pythonEnvLog = nullptr;
};
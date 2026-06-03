#pragma once

#include <QWidget>
#include <QPointer>
#include <QToolBar>
#include <QPrinter>
#include <QPainter>
#include <QPixmap>
#include <QEventLoop>

#include <memory>

class QBaseView : public QWidget
{
    Q_OBJECT
    Q_PROPERTY( QString title READ title NOTIFY sigTitleChanged )

public:
    // ── 테마 ──
    enum class Theme { Light, Dark };
    Q_ENUM( Theme )

    explicit QBaseView( QWidget* Parent = nullptr );
    ~QBaseView() override;

    QString                             title() const;
    QString                             currentFilePath() const { return m_filePath; }

    // ── 파일 관리 (순수 가상) ──
    virtual bool                        openFile( const QString& filePath ) = 0;
    virtual bool                        saveFile( const QString& filePath = {} ) = 0;
    virtual void                        closeFile() = 0;
    virtual bool                        isModified() const = 0;
    virtual QStringList                 supportedExtensions() const = 0;
    virtual bool                        opensFileAsynchronously() const { return false; }
    virtual bool                        canCancelLoading() const { return opensFileAsynchronously() && isLoading(); }

    // ── 복사 ── 클립보드에 복사 가능 여부
    virtual bool                        canCopyToClipboard() const { return false; }
    virtual bool                        copySelectionToClipboard() { return false; }

    ///////////////////////////////////////////////////////////////////////////
    /// 뷰 독립 도구모음
    virtual QToolBar*                   createToolBar();
    virtual QToolBar*                   createAuxiliaryToolBar() { return nullptr; }
    void                                setToolBarVisible( bool visible );
    bool                                isToolBarVisible() const { return m_toolBarVisible; }

    // ── 테마 ──
    virtual void                        setTheme( Theme theme );
    Theme                               currentTheme() const { return m_theme; }

    bool                                isShuttingDown() const { return m_shuttingDown; }
    bool                                isLoading() const { return m_loadingActive; }

signals:
    void                                sigFileOpened( const QString& path );
    void                                sigFileOpenFailed( const QString& path, const QString& errorMessage );
    void                                sigFileClosed();
    void                                sigModifiedChanged( bool modified );
    void                                sigTitleChanged( const QString& title );
    void                                sigCopyAvailabilityChanged( bool available );
    void                                sigLoadingStateChanged( bool active, const QString& message, int value, int maximum );

protected:
    void                                setDisplayTitle( const QString& title );
    void                                applyThemeStyleSheet( Theme theme );
    void                                updateCopyAvailability();

    void                                beginLoading( const QString& message, int maximum = 0 );
    void                                updateLoadingProgress( const QString& message, int value = -1, int maximum = -1, QEventLoop::ProcessEventsFlags flags = QEventLoop::ExcludeUserInputEvents );
    void                                endLoading();

    ///////////////////////////////////////////////////////////////////////////
    ///
    
    // 데이터
    QString                             m_filePath;
    QString                             m_displayTitle;
    Theme                               m_theme = Theme::Light;
    QPointer<QToolBar>                  m_toolBar = nullptr;
    QPointer<QToolBar>                  m_auxiliaryToolBar = nullptr;
    bool                                m_toolBarVisible = true;
    bool                                m_lastCopyAvailability = false;
    bool                                m_loadingActive = false;
    QString                             m_loadingMessage;
    int                                 m_loadingValue = 0;
    int                                 m_loadingMaximum = 0;
    bool                                m_shuttingDown = false;
};

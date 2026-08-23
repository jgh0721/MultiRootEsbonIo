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

    // ── 붙여넣기 ── 클립보드에서 붙여넣기 가능 여부
    virtual bool                        canPasteFromClipboard() const { return false; }
    virtual bool                        pasteFromClipboard() { return false; }

    ///////////////////////////////////////////////////////////////////////////
    /// 뷰 독립 도구모음
    virtual QToolBar*                   createToolBar();
    virtual QToolBar*                   createAuxiliaryToolBar() { return nullptr; }
    /// 만들어지는 도구모음의 부모가 될 위젯. MainWindow 가 자기 도구모음 슬롯을
    /// 심어 준다.
    ///
    /// 왜 뷰 자신을 부모로 쓰지 않는가: 뷰는 도크 매니저 안에 있고, Qt ADS 는
    /// 도크 매니저에 스타일시트를 건다. 스타일시트가 조상에 있으면 Qt 는 위젯을
    /// QStyleSheetStyle 로 감싸 **생성 도중에** polish() 를 부르는데, 그 자리에서
    /// Qlementine 의 ComboboxItemViewFilter 가 무한 재귀에 빠진다(0xC00000FD).
    /// 이 프로젝트가 이미 두 번 물린 상류 버그다 — solThemeManager.cpp 의 전역
    /// 스타일시트 금지 주석과 QTextView::createToolBar() 의
    /// setSizeAdjustPolicy 금지 주석이 같은 것을 말한다. 도구모음 슬롯은 도크
    /// 매니저의 형제라 그 사슬 밖에 있다.
    ///
    /// 도구모음이 만들어진 **뒤에** 부모를 옮기는 것으로는 늦다. 콤보박스가
    /// 생성되는 시점이 문제이기 때문이다.
    void                                setToolBarHost( QWidget* host ) { m_toolBarHost = host; }
    void                                setToolBarVisible( bool visible );
    bool                                isToolBarVisible() const { return m_toolBarVisible; }

    /// 이 뷰에서 **실제로 글자를 받는 위젯**에 키보드 포커스를 준다.
    ///
    /// setFocus() 로는 모자란다. 뷰는 껍데기이고 입력을 받는 것은 그 안의
    /// Scintilla 위젯이라, 껍데기에 포커스를 주면 키가 어디로도 가지 않는다.
    /// 기본 구현은 자기 자신이고, 안쪽에 진짜 입력 위젯이 있는 뷰가 덮는다.
    virtual void                        focusContent() { setFocus( Qt::OtherFocusReason ); }

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
    /// setToolBarHost() 참고. 비어 있으면 뷰 자신이 부모가 된다.
    QPointer<QWidget>                   m_toolBarHost = nullptr;
    bool                                m_toolBarVisible = true;
    bool                                m_lastCopyAvailability = false;
    bool                                m_loadingActive = false;
    QString                             m_loadingMessage;
    int                                 m_loadingValue = 0;
    int                                 m_loadingMaximum = 0;
    bool                                m_shuttingDown = false;
};

#pragma once

#include "core/solBaseView.hpp"
#include "core/solSphinxDiagnostics.hpp"

#include "ScintillaDocument.hpp"
#include "ScintillaEditorSettings.hpp"
#include "TextFileSession.hpp"

#include <QPoint>
#include <QPointer>
#include <QVector>
#include <QPair>
#include <atomic>

class ScintillaQtDirectBackend;
class FindReplaceWidget;

/// Scintilla 백엔드 기반 텍스트/코드 뷰어
class QTextView : public QBaseView
{
    Q_OBJECT

public:
    enum LineEnding { LF, CRLF, CR };
    Q_ENUM( LineEnding )

        explicit QTextView( QWidget* parent = nullptr );
    ~QTextView() override;

    // ── QBaseView ──
    bool        openFile( const QString& filePath ) override;
    bool        saveFile( const QString& filePath ) override;
    void        closeFile() override;
    bool        isModified() const override;
    QStringList supportedExtensions() const override;
    bool        opensFileAsynchronously() const override;

    // ── 인코딩 ──
    QString       detectedEncoding() const;
    QString       currentEncoding() const;
    QString       currentEncodingDisplayName() const;
    void          reloadWithEncoding( const QString& encoding );
    bool          saveWithEncoding( const QString& filePath, const QString& encoding, bool includeBom );
    bool          saveFileAs();
    static QStringList availableEncodings();

    // ── 글꼴 ──
    void  setEditorFont( const QFont& font );
    QFont editorFont() const;
    double lineSpacingScale() const;
    void setLineSpacingScale( double scale );

    // ── 구문 강조 ──
    void    setLanguage( const QString& language );
    QString currentLanguage() const;
    void    autoDetectLanguage();

    // ── 편집 ──
    void setReadOnly( bool on );
    bool isReadOnly() const;
    bool isLimitedPreviewMode() const;
    bool isContentTruncated() const;
    QString contentLoadModeText() const;
    bool isHotExitEnabled() const;
    void setHotExitEnabled( bool enabled );
    bool openHotExitBackup( const QString& untitledId );
    void flushHotExitBackup();
    void discardHotExitBackup();
    bool canCopyToClipboard() const override;
    bool copySelectionToClipboard() override;
    void goToLine( int line );
    void goToPosition( int line, int column );
    int  characterCount() const;
    int  tabWidth() const;
    void setTabWidth( int width );
    bool useTabs() const;
    void setUseTabs( bool use );
    bool indentationGuidesVisible() const;
    void setIndentationGuidesVisible( bool visible );
    ScintillaEditorSettings::IndentGuideStyle indentGuideStyle() const;
    void setIndentGuideStyle( ScintillaEditorSettings::IndentGuideStyle style );
    int  selectedCharacterCount() const;
    int  lineCount() const;
    int  currentLine() const;
    int  currentColumn() const;

    // ── 검색 / 치환 ──
    void findText( const QString& text,
                  bool regex = false,
                  bool caseSensitive = false,
                  bool wholeWords = false,
                  bool forward = true );
    void replaceText( const QString& find,
                     const QString& replace,
                     bool regex = false,
                     bool caseSensitive = false,
                     bool wholeWords = false,
                     bool forward = true );
    void replaceAll( const QString& find,
                    const QString& replace,
                    bool regex = false,
                    bool caseSensitive = false,
                    bool wholeWords = false,
                    bool forward = true );

    // ── 찾기/바꾸기 바 ──
    void showFindBar( bool replaceMode = false );
    void hideFindBar();

    // ── 줄바꿈 ──
    LineEnding detectedLineEnding() const;
    void       setLineEnding( LineEnding ending );
    bool       isWordWrapEnabled() const;
    void       setWordWrap( bool enabled );

    // ── 폰트 렌더링 ──
    ScintillaEditorSettings::FontRenderingMode fontRenderingMode() const;
    void setFontRenderingMode( ScintillaEditorSettings::FontRenderingMode mode );

    // ── 눈금자 ──
    bool isRulerVisible() const;
    void setRulerVisible( bool visible );

    // ── 공백/개행 문자 표시 ──
    bool isWhitespaceVisible() const;
    void setWhitespaceVisible( bool visible );

    // ── 코드 폴딩 ──
    bool isCodeFoldingEnabled() const;
    void setCodeFoldingEnabled( bool enabled );

    // ── 괄호 강조 ──
    bool isBraceHighlightEnabled() const;
    void setBraceHighlightEnabled( bool enabled );

    // ── Change History ──
    ScintillaEditorSettings::ChangeHistoryMode changeHistoryMode() const;
    void setChangeHistoryMode( ScintillaEditorSettings::ChangeHistoryMode mode );

    // ── Sphinx/Esbonio 서비스 계층용 접근자 ──
    // LSP 문서 동기화, 자동완성 컨텍스트, 프리뷰 스크롤 동기화, 진단 표시가
    // 전부 이 API 위에 올라간다. 문서 텍스트와 캐럿을 밖에서 물어볼 수 있어야 한다.
    [[nodiscard]] QString text() const;
    [[nodiscard]] QString lineText( int line ) const;
    [[nodiscard]] QString textRange( int startPos, int endPos ) const;
    [[nodiscard]] int     positionFromLineColumn( int line, int column ) const;
    [[nodiscard]] int     currentPosition() const;
    [[nodiscard]] int     firstVisibleLine() const;
    void                  scrollToLine( int line, double viewportRatio = 0.35 );
    /// 캐럿의 전역 화면 좌표. 자동완성 팝업 위치 기준점.
    [[nodiscard]] QPoint  caretGlobalPos() const;
    /// 커서 바로 앞 backspaceCount 글자를 지우고 insertText 를 넣는다.
    /// LSP 완성 항목 삽입용.
    void                  replaceRangeAtCursor( int backspaceCount, const QString& insertText );
    /// 진단 스퀴글을 다시 그린다. 빈 목록이면 전부 지운다.
    void                  setDiagnosticMarks( const QVector< mrst::DiagnosticEntry >& entries );
    [[nodiscard]] QString diagnosticTooltipAt( int position ) const;

    QToolBar* createToolBar() override;
    void setTheme( Theme theme ) override;

signals:
    void encodingChanged( const QString& encoding );
    void languageChanged( const QString& language );
    void statusChanged();

    /// 사용자가 문서를 편집했다 (파일 로드로 인한 변경은 제외).
    void sigTextEdited();
    /// 캐럿이 이동했다. 1-based 줄/열.
    void sigCursorMoved( int line, int column );
    /// 사용자가 문자를 입력했다. 자동완성 트리거 문자 감지용.
    void sigCharAdded( int ch );

private:
    struct SearchOptions
    {
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWords = false;
        bool forward = true;
    };

    bool promptOpenModeForFile( const QString& filePath, TextFileSession::OpenMode& openMode ) const;
    bool ensureEditorBackend();
    void loadPersistedEditorPreferences();
    void applyPersistedEditorPreferences( ScintillaEditorSettings& settings ) const;
    void applyEditorSettings();
    void showGoToLineDialog();
    void updateMetrics();
    static ScintillaDocument::LineEnding toDocumentLineEnding( LineEnding ending );
    static LineEnding fromDocumentLineEnding( ScintillaDocument::LineEnding ending );
    static LineEnding detectLineEnding( const QString& text );
    static QString newHotExitUntitledId();
    bool saveWithEncodingDialog( const QString& initialFilePath = {} );
    bool currentFileHasBom() const;
    bool shouldUseHotExitForCurrentFile() const;
    void scheduleHotExitBackup();
    void writeHotExitBackupNow( bool synchronous = false );
    void onHotExitBackupTimer();
    void applyHotExitSettingsFromPreferences();

    QPointer<ScintillaQtDirectBackend> m_editor = nullptr;
    class ColumnRulerWidget* m_ruler = nullptr;
    FindReplaceWidget* m_findWidget = nullptr;
    TextFileSession       m_fileSession;
    ScintillaDocument     m_document;
    ScintillaEditorSettings m_editorSettings;

    QString             m_encoding;
    QString             m_detectedEncoding;
    QString             m_hotExitUntitledId;
    SearchOptions       m_searchOptions;
    std::atomic_int     m_openRequestId{ 0 };
    int                 m_cachedLineCount = 1;
    int                 m_cachedCurrentLine = 1;
    int                 m_cachedCurrentColumn = 1;
    class QTimer* m_hotExitTimer = nullptr;
    bool                m_hotExitEnabled = true;
    bool                m_hotExitDirty = false;

    // ── 제외 기능 ──
    static constexpr int kExcludeBackgroundIndicatorId = 20;
    static constexpr int kCurrentMatchIndicatorId = 21;
    static constexpr int kExcludeStrikeIndicatorId = 22;
    static constexpr int kExcludeBorderIndicatorId = 23;
    QVector<QPair<int, int>> m_excludedRanges; // (startPos, length)
    bool m_isSearching = false; // 검색 중 selectionChanged 무시 플래그
    class QTimer* m_searchDebounceTimer = nullptr;
    int m_currentMatchStart = -1;
    int m_currentMatchLength = 0;
    int m_searchResumePos = -1;

    // ── 진단 표시 ──
    // 20~23 은 검색 제외 기능이 이미 쓰고 있어 12~15 를 진단에 배정한다.
    static constexpr int kDiagnosticErrorIndicatorId = 12;
    static constexpr int kDiagnosticWarningIndicatorId = 13;
    static constexpr int kDiagnosticInfoIndicatorId = 14;
    static constexpr int kDiagnosticHintIndicatorId = 15;
    QVector< mrst::DiagnosticEntry > m_diagnostics;
    bool                m_diagnosticIndicatorsReady = false;

    void configureDiagnosticIndicators();
    [[nodiscard]] static int diagnosticIndicatorFor( int severity );

    void setupFindWidget();
    void connectFindWidgetSignals();
    void performSearch();
    void performFindNext();
    void performFindPrev();
    void performReplace();
    void performReplaceAll();
    void performExclude();
    void clearExcludedRanges();
    void updateExcludedCount();
    void clearCurrentMatchHighlight();
    void setCurrentMatchHighlight( int startPos, int length, bool scrollToMatch );
    int currentSearchFlags() const;
    bool isExcludedRange( int startPos, int length ) const;
    bool findInRange( int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary = false );
    bool findInSelectionRange( bool forward, bool wrap, bool fromRangeBoundary = false );
    bool hasCurrentMatch() const;
    void updateMatchCount();
};

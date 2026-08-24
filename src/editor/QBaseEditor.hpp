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
    /// 키보드 포커스를 Scintilla 위젯으로 보낸다. 껍데기인 이 뷰가 받으면
    /// 글자가 어디로도 가지 않는다.
    void        focusContent() override;

    // ── 인코딩 ──
    QString       detectedEncoding() const;
    QString       currentEncoding() const;
    QString       currentEncodingDisplayName() const;
    void          reloadWithEncoding( const QString& encoding );
    bool          saveWithEncoding( const QString& filePath, const QString& encoding, bool includeBom );
    bool          saveFileAs();

    // ── 외부 변경 ──
    /// 디스크의 내용으로 본문을 다시 채운다. 캐럿과 스크롤 위치는 지킨다.
    ///
    /// **저장하지 않은 편집이 있는지 여기서 따지지 않는다.** 그 판단(무시/자동
    /// 불러오기/묻기)은 설정을 읽고 대화상자를 띄울 수 있는 MainWindow 의 몫이고,
    /// 여기까지 왔다면 이미 덮어써도 된다는 결정이 난 것이다.
    bool          reloadFromDisk();
    /// 파일이 밖에서 사라졌다. 본문은 그대로 두고 "수정됨" 으로 표시한다.
    ///
    /// 탭을 닫지도, 본문을 비우지도 않는다. 사용자에게 남은 유일한 사본이 이
    /// 버퍼이기 때문이다. 수정됨 표시가 붙어 있으면 탭을 닫을 때 저장을 묻는
    /// 평소 경로가 그 사본을 지켜 준다.
    void          markFileVanished();
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
    bool flushHotExitBackup();
    void discardHotExitBackup();
    void abandonHotExitBackup();
    bool canCopyToClipboard() const override;
    bool copySelectionToClipboard() override;
    bool canPasteFromClipboard() const override;
    bool pasteFromClipboard() override;
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

    // ── 줄바꿈(EOL) ──
    LineEnding detectedLineEnding() const;
    void       setLineEnding( LineEnding ending );

    // ── 자동 줄넘김(wrap) ──
    ScintillaEditorSettings::WrapMode wordWrapMode() const;
    void setWordWrapMode( ScintillaEditorSettings::WrapMode mode );
    /// Alt+Z. 켜져 있으면 끄고, 꺼져 있으면 마지막으로 고른 모드로 되돌린다.
    void toggleWordWrap();
    int  wrapVisualFlags() const;
    void setWrapVisualFlags( int flags );
    ScintillaEditorSettings::WrapIndentMode wrapIndentMode() const;
    void setWrapIndentMode( ScintillaEditorSettings::WrapIndentMode mode );

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
    /// 문서 전체를 접거나 펼친다. 접기가 꺼져 있으면 아무 일도 하지 않는다.
    void foldAll( bool contract );

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
    /// 캐럿의 줄/열 (1-based). currentLine()/currentColumn() 과 달리 캐시를
    /// 거치지 않고 Scintilla 에 직접 묻는다.
    ///
    /// 캐시는 SCN_UPDATEUI 로 갱신되는데 SCN_CHARADDED 가 그보다 먼저 도착한다.
    /// 문자 입력 직후에 캐시를 읽으면 한 글자(연속 입력이면 여러 글자) 이전
    /// 위치가 나와 자동완성 컨텍스트가 통째로 어긋난다.
    [[nodiscard]] int     caretLine() const;
    [[nodiscard]] int     caretColumn() const;
    [[nodiscard]] int     firstVisibleLine() const;
    /// 화면 맨 위에 보이는 **문서** 줄 (1-based). firstVisibleLine() 은 화면 행이라
    /// 자동 줄넘김이 켜지면 창 폭에 따라 달라진다. 세션에 저장할 값은 이쪽이다.
    [[nodiscard]] int     topDocumentLine() const;
    void                  scrollToLine( int line, double viewportRatio = 0.35 );
    /// 창 세로 ratio 위치에 실제로 보이는 줄 (1-based, 소수).
    /// 12.5 는 12번 줄의 세로 중간을 뜻한다 — 자동 줄바꿈으로 여러 행을
    /// 차지하는 줄 안에서의 위치까지 표현한다.
    [[nodiscard]] double  fractionalLineAtViewportRatio( double ratio ) const;
    /// 그 (소수) 줄 위치가 창 세로 ratio 위치에 오도록 스크롤한다.
    void                  scrollFractionalLineToViewportRatio( double fractionalLine, double ratio );
    /// 캐럿의 전역 화면 좌표. 자동완성 팝업 위치 기준점.
    [[nodiscard]] QPoint  caretGlobalPos() const;
    /// 커서 바로 앞 backspaceCount 글자를 지우고 insertText 를 넣는다.
    /// LSP 완성 항목 삽입용.
    void                  replaceRangeAtCursor( int backspaceCount, const QString& insertText );
    /// 진단 스퀴글을 다시 그린다. 빈 목록이면 전부 지운다.
    void                  setDiagnosticMarks( const QVector< mrst::DiagnosticEntry >& entries );
    /// LSP 자동완성에서 수확한 directive/role 이름을 reST 렉서 캐시에 먹인다.
    /// 캐시가 채워져야 목록에 없는 directive 가 비로소 빨갛게 표시된다 (3-state).
    void                  feedRstCompletionVocabulary( const QStringList& directives,
                                                       const QStringList& roles );
    [[nodiscard]] QString diagnosticTooltipAt( int position ) const;

    QToolBar* createToolBar() override;
    void setTheme( Theme theme ) override;

signals:
    void encodingChanged( const QString& encoding );
    void languageChanged( const QString& language );
    void statusChanged();

    /// 이 파일에 우리가 쓰기를 시작했다 / 다 썼다.
    ///
    /// 외부 변경 감시가 자기 저장을 남의 편집으로 오해하지 않게 하는 데 쓴다.
    /// 저장 경로가 넷(액션, 도구모음, 인코딩 대화상자, 다른 이름으로 저장)이지만
    /// 전부 saveWithEncoding() 을 지나므로 신호는 거기 한 곳에서만 낸다.
    void sigFileWriteStarted( const QString& filePath );
    void sigFileSaved( const QString& filePath );
    /// 디스크 내용으로 본문을 다시 채웠다. 프리뷰·LSP·개요를 다시 맞춰야 한다.
    void sigFileReloadedFromDisk( const QString& filePath );

    /// 사용자가 문서를 편집했다 (파일 로드로 인한 변경은 제외).
    void sigTextEdited();
    /// 편집 구간. 좌표는 LSP 규약 그대로(0-based 줄, 줄머리로부터의 바이트 수).
    /// LSP 증분 동기화에 쓴다. sigTextEdited 보다 **먼저** 나간다.
    void sigDocumentEdited( int startLine, int startColumn, int oldEndLine, int oldEndColumn,
                            const QByteArray& newText );
    /// 캐럿이 이동했다. 1-based 줄/열.
    void sigCursorMoved( int line, int column );
    /// 사용자가 문자를 입력했다. 자동완성 트리거 문자 감지용.
    void sigCharAdded( int ch );
    /// 세로 스크롤이 변했다. 프리뷰 동기화용.
    void sigViewportScrolled();
    /// 마우스가 `:role:`target`` 위에서 멈췄다. globalPos 는 전역 화면 좌표.
    /// 롤 이름에 콜론이 들어가는 도메인 표기(`py:func`)도 그대로 넘어온다.
    void sigRoleHovered( const QString& role, const QString& target, const QPoint& globalPos );
    /// 호버가 끝났다 (마우스가 움직였거나 편집기를 벗어났다).
    void sigRoleHoverEnded();
    /// 자동 줄넘김 모드가 바뀌었다 (ScintillaEditorSettings::WrapMode).
    /// 도구모음 콤보가 Alt+Z / 설정 변경을 따라오게 하는 용도.
    void sigWordWrapModeChanged( int mode );

private:
    struct SearchOptions
    {
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWords = false;
        bool forward = true;
    };

    /// dwell 위치의 줄에서 `:role:`target`` 을 찾아 sigRoleHovered 를 낸다.
    /// 커서가 롤 안에 없으면 sigRoleHoverEnded 를 낸다.
    void handleDwellStart( int position, const QPoint& viewportPos );

    bool promptOpenModeForFile( const QString& filePath, TextFileSession::OpenMode& openMode ) const;
    /// reloadFromDisk() 의 GUI 스레드 뒷부분. 읽기가 성공했을 때만 불린다.
    void applyReloadedContent( TextFileSession session,
                              const QString& text,
                              bool encodingOverridden,
                              const QString& overriddenEncoding,
                              int caretPosition,
                              int topLine );
    bool ensureEditorBackend();
    void loadPersistedEditorPreferences();
    void applyPersistedEditorPreferences( ScintillaEditorSettings& settings ) const;
    void applyEditorSettings();
    /// 줄넘김 3종만 편집기에 밀어 넣고 열 눈금자를 되맞춘다.
    void applyWrapSettingsToEditor();
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
    bool writeHotExitBackupNow( bool synchronous = false );
    void onHotExitBackupTimer();
    void applyHotExitSettingsFromPreferences();

    QPointer<ScintillaQtDirectBackend> m_editor = nullptr;
    /// createToolBar() 가 만든 도구모음. 소유권은 이것을 받아 간 쪽(MainWindow)
    /// 에 있고, 테마가 바뀔 때 라벨 색을 다시 칠하려고 들고만 있는다.
    QPointer<QToolBar>  m_toolBar;
    class ColumnRulerWidget* m_ruler = nullptr;
    FindReplaceWidget* m_findWidget = nullptr;
    TextFileSession       m_fileSession;
    ScintillaDocument     m_document;
    ScintillaEditorSettings m_editorSettings;
    /// Alt+Z 로 줄넘김을 껐을 때 되돌아갈 모드. WrapNone 은 담기지 않는다.
    ScintillaEditorSettings::WrapMode m_lastWrapMode = ScintillaEditorSettings::WrapChar;

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
    /// 저장하지 않고 명시적으로 닫힌 문서. 이 문서가 뷰에서 사라질 때까지
    /// hot exit 백업을 다시 만들지 않는다. abandonHotExitBackup() 참고.
    bool                m_hotExitAbandoned = false;

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
    /// 파일 로드/복원으로 본문을 채우는 동안은 사용자의 편집이 아니다.
    /// 이 플래그가 켜져 있으면 sigTextEdited 를 내보내지 않는다.
    bool                m_applyingFileContent = false;

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

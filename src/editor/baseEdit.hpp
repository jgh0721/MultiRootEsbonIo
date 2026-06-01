#pragma once

#include "ScintillaEdit.hpp"

class FindReplaceWidget;

class BaseEdit : public QWidget
{
    Q_OBJECT
public:
    explicit BaseEdit( QWidget* Parent = nullptr );
    ~BaseEdit() override;

    QScintillaEdit*                     Scintilla() const;
    QWidget*                            EditorWidget() const;

    QString                             FilePath() const;
    QString                             NormalizedFilePath() const;
    QString                             DisplayName() const;

    bool                                LoadFile( const QString& FilePath, QString* ErrorMessage = nullptr );
    bool                                SaveFile( QString* ErrorMessage = nullptr );
    bool                                SaveFileAs( const QString& FilePath, QString* ErrorMessage = nullptr );

    virtual QString                     EditorType() const;

    bool                                IsAutoCompletionAvailable() const;
    bool                                IsPreviewAvailable() const;
    bool                                IsOutlineAvailable() const;
    bool                                IsDiagnosticsAvailable() const;

    void                                SetRulerVisible( bool Visible );
    bool                                IsRulerVisible() const;
    void                                RefreshRuler();

    void                                SetReadOnly( bool ReadOnly );
    bool                                IsReadOnly() const;

    // ── 검색 / 치환 ──
    void                                ShowFindBar( bool replaceMode = false );
    void                                HideFindBar();
    void                                FindText( const QString& text, bool regex = false, bool caseSensitive = false, bool wholeWords = false, bool forward = true );
    void                                ReplaceText( const QString& find, const QString& replace, bool regex = false, bool caseSensitive = false, bool wholeWords = false, bool forward = true );
    void                                ReplaceAll( const QString& find, const QString& replace, bool regex = false, bool caseSensitive = false, bool wholeWords = false, bool forward = true );

signals:
    void                                filePathChanged( const QString& filePath );
    void                                modificationChanged( bool modified );
    void                                cursorPositionChanged( int line, int index );
    void                                linesChanged();
    void                                textChanged();
    void                                selectionChanged();

protected:
    void                                SetAutoCompletionAvailable( bool Available );
    void                                SetPreviewAvailable( bool Available );
    void                                SetOutlineAvailable( bool Available );
    void                                SetDiagnosticsAvailable( bool Available );

private:
    static QString                      normalizeFilePath( const QString& FilePath );
    void                                setFilePath( const QString& FilePath );

    QScintillaEdit*                     m_scintilla = nullptr;
    QWidget*                            m_rulerWidget = nullptr;
    QString                             m_filePath;
    QString                             m_normalizedFilePath;
    bool                                m_autoCompletionAvailable = false;
    bool                                m_previewAvailable = false;
    bool                                m_outlineAvailable = false;
    bool                                m_diagnosticsAvailable = false;


    /// 검색 관련 
    void                                setupFindWidget();
    void                                connectFindWidgetSignals();
    void                                performSearch();
    void                                performFindNext();
    void                                performFindPrev();
    void                                performReplace();
    void                                performReplaceAll();
    void                                performExclude();
    void                                clearExcludedRanges();
    void                                updateExcludedCount();
    void                                clearCurrentMatchHighlight();
    void                                setCurrentMatchHighlight( int startPos, int length, bool scrollToMatch );
    int                                 currentSearchFlags() const;
    bool                                isExcludedRange( int startPos, int length ) const;
    bool                                findInRange( int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary = false );
    bool                                findInSelectionRange( bool forward, bool wrap, bool fromRangeBoundary = false );
    bool                                hasCurrentMatch() const;
    void                                updateMatchCount();

    struct SearchOptions
    {
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWords = false;
        bool forward = true;
    };

    static constexpr int                kExcludeBackgroundIndicatorId = 20;
    static constexpr int                kCurrentMatchIndicatorId = 21;
    static constexpr int                kExcludeStrikeIndicatorId = 22;
    static constexpr int                kExcludeBorderIndicatorId = 23;
    QVector<QPair<int, int>>            m_excludedRanges; // (startPos, length)
    bool                                m_isSearching = false; // 검색 중 selectionChanged 무시 플래그
    class QTimer*                       m_searchDebounceTimer = nullptr;
    int                                 m_currentMatchStart = -1;
    int                                 m_currentMatchLength = 0;
    int                                 m_searchResumePos = -1;
    SearchOptions                       m_searchOptions;
    FindReplaceWidget*                  m_findWidget = nullptr;
};


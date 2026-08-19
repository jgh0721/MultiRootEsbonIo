#pragma once

#include "ScintillaDocument.hpp"
#include "RstContainerLexer.hpp"

#include <QFont>
#include <QPointer>

#include <memory>

class ScintillaEditBase;

namespace Scintilla {
class ILexer5;
}

struct ScintillaEditorSettings;

class ScintillaQtDirectBackend : public QObject
{
	Q_OBJECT

public:
	explicit ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent = nullptr);
	~ScintillaQtDirectBackend() override;

	QWidget* widget() const;
	QAbstractScrollArea* scrollArea() const;

	void applySettings(const ScintillaEditorSettings& settings);
	/// 자동 줄넘김 관련 메시지만 보낸다. applySettings() 는 끝에서 문서 전체를
	/// 다시 렉싱하므로(SCI_COLOURISE + 접기 깊이 재계산) 줄넘김만 바뀌는
	/// 경로에서 그 비용을 치를 이유가 없다.
	void applyWrapSettings(const ScintillaEditorSettings& settings);
	void setText(const QString& text);
	QString text() const;
	void clear();

	void setModified(bool modified);
	bool isModified() const;
	int lineCount() const;

	void setReadOnly(bool readOnly);
	bool isReadOnly() const;

	bool hasSelectedText() const;
	QString selectedText() const;
	bool getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const;
	void setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo);
	void setSelectionByPos(int startPos, int endPos);
	void copy();
	void paste();
	bool canPaste() const;

	void setCursorPosition(int line, int index);
	bool findFirst(const QString& text,
					   bool regex,
					   bool caseSensitive,
					   bool wholeWords,
					   bool wrap,
					   bool forward);
	void replace(const QString& replacement);

	int countMatches(const QString& text,
					 bool regex,
					 bool caseSensitive,
					 bool wholeWords);
	int countMatchesInRange(const QString& text,
							bool regex,
							bool caseSensitive,
							bool wholeWords,
							int startPos,
							int endPos);
	void setIndicatorStyle(int indicatorId, int style, const QColor& color);
	void applyIndicator(int indicatorId, int startPos, int length);
	void clearIndicator(int indicatorId, int startPos, int length);
	void clearAllIndicator(int indicatorId);
	int selectionStartPos() const;
	int selectionEndPos() const;
	int documentLength() const;
	int currentPos() const;
	int matchStartPos() const;
	int matchEndPos() const;
	void scrollRangeToView(int startPos, int endPos);

	void setSearchTargetRange(int startPos, int endPos);
	int searchInTarget(const QString& text, int flags);
	int targetEnd() const;
	void replaceInTarget(const QString& text);

	void setEditorFont(const QFont& font);
	QFont editorFont() const;

	int firstVisibleLine() const;
	/// 화면 맨 위 행이 속한 문서 줄 (0-based). 줄넘김이 켜져 있어도 창 폭에
	/// 흔들리지 않으므로 세션/핫 엑시트 저장에는 이 값을 쓴다.
	int topDocumentLine() const;
	void setFirstVisibleLine(int line);
	int linesOnScreen() const;

	// ── 뷰포트 비율 기준 스크롤 ──
	//
	// 전부 픽셀 좌표로 계산한다. 문서 줄 번호로 계산하면 자동 줄바꿈이나 코드
	// 접기가 있을 때 어긋난다 (한 문서 줄이 화면에서는 여러 줄을 차지하거나
	// 아예 안 보일 수 있다).
	//
	// 반환/입력은 "소수 줄 번호" 다. 12.5 는 12번 줄의 세로 중간 지점을 뜻하며,
	// 줄바꿈으로 여러 행을 차지하는 줄 안에서의 위치까지 표현할 수 있다.
	double fractionalLineAtViewportRatio(double ratio) const;
	void scrollFractionalLineToViewportRatio(double fractionalLine, double ratio);
	void restoreViewState(int caretPosition, int firstVisibleLine);

	// ── 위치 변환 ──
	// Scintilla 위치는 UTF-8 바이트 오프셋이고 열은 코드 유닛 단위다.
	int positionFromLine(int line) const;
	int lineEndPosition(int line) const;
	int lineFromPosition(int position) const;
	int columnFromPosition(int position) const;
	int positionFromLineColumn(int line, int column) const;
	QByteArray textRangeUtf8(int startPos, int endPos) const;
	QString lineText(int line) const;
	QPoint pointFromPosition(int position) const;
	/// 뷰포트 좌표 -> 문서 위치. 글자 위가 아니면 -1.
	int positionFromPoint(const QPoint& viewportPos) const;

	// ── 컨테이너 렉싱 지원 ──
	// ILexer 를 비운 상태에서 styleNeeded 시그널을 받아 직접 스타일을 칠할 때 쓴다.
	int endStyled() const;
	void startStyling(int position);
	void setStylingEx(const QByteArray& styleBytes);
	void colouriseAll();
	void setStyleForeground(int style, const QColor& color);
	void setStyleBackground(int style, const QColor& color);
	void setStyleBold(int style, bool bold);
	void setStyleItalic(int style, bool italic);
	void setStyleUnderline(int style, bool underline);
	int textHeight(int line) const;
	int leftMarginWidth() const;
	int changeHistoryFlags() const;
	void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting);
	bool applyLanguage(const QString& displayName);
	void applyThemeColors(bool dark);

	/// reST 컨테이너 렉서의 메타데이터 캐시. Esbonio 자동완성 결과를 여기에
	/// 넣으면 directive/role 이 UNKNOWN -> VALID/INVALID 로 바뀐다.
	/// 컨테이너 렉싱 중이 아니면 nullptr.
	[[nodiscard]] mrst::rst::RstMetadataCache* rstMetadataCache() const;
	void restyleDocument();

	// ── 코드 접기 ──
	/// 그 문서 줄이 지금 화면에 보이는가 (접혀 있지 않은가).
	[[nodiscard]] bool isLineVisible(int line) const;
	/// 접혀서 안 보이는 줄이면 그 줄을 감추고 있는, 화면에 보이는 조상 줄.
	/// 보이는 줄이면 그대로 돌려준다.
	[[nodiscard]] int visibleAnchorLine(int line) const;
	/// 그 줄이 접힌 블록 안에 있으면 펼쳐서 보이게 한다.
	void ensureLineVisible(int line);
	/// 전체 접기/펼치기. 개요나 단축키에서 부른다.
	void foldAll(bool contract);

signals:
	void modificationChanged(bool modified);
	void cursorPositionChanged(int line, int index);
	void linesChanged();
	void textChanged();
	void selectionChanged();
	/// 컨테이너 렉싱 모드에서 Scintilla 가 스타일을 요구하는 구간의 끝 위치.
	/// SCI_SETIDLESTYLING 때문에 문서 전체가 아니라 청크 단위로 도착한다.
	void styleNeeded(int endPosition);
	/// 사용자가 문자를 입력했다. 자동완성 트리거 감지에 쓴다.
	void charAdded(int ch);
	/// 세로 스크롤이 변했다. 프리뷰 동기화에 쓴다.
	void viewportScrolled();
	/// 마우스가 글자 위에서 멈췄다. position 은 문서 위치, viewportPos 는 뷰포트 좌표.
	void dwellStarted(int position, const QPoint& viewportPos);
	void dwellEnded();

private:
	void configureCodeFolding(bool enabled);
	void configureBraceHighlightIndicators();
	void syncLineCountState(bool emitSignal);
	void updateLineNumberMargin(int minimumDigits = 0);
	void emitCursorAndSelectionState();
	void updateBraceHighlight();
	void clearLexer();
	void applySyntaxStyles(bool dark);
	void applyRstSyntaxStyles();
	void applyMarkdownSyntaxStyles();
	void setKeywordsForLexer(const QString& lexerKey);
	void handleStyleNeeded(int endPosition);
	/// reST 는 Lexilla 렉서가 없어 접기 깊이도 우리가 계산해 넣는다.
	/// 섹션 깊이는 문서 앞부분 전체에 의존하므로 문서 단위로 다시 센다.
	void updateRstFoldLevels();
	void scheduleRstFoldUpdate();

	QPointer<ScintillaEditBase> m_editor;
	std::unique_ptr<mrst::rst::RstContainerLexer> m_rstLexer;   // 컨테이너 렉싱 중에만 유효
	Scintilla::ILexer5*         m_currentLexer = nullptr; // Non-owning after SCI_SETILEXER while the editor is alive; cleared when the editor dies.
	QFont               m_editorFont;
	QString             m_currentLexerKey;
	bool                m_darkTheme = false;
	bool                m_codeFoldingEnabled = true;
	bool                m_braceMatchingEnabled = true;
	bool                m_highlightCurrentLine = true;
	bool                m_forcedModified = false;
	int                 m_lineNumberMarginDigits = 0;
	int                 m_lastKnownLineCount = 1;
	/// 편집 중에 매 글자 문서 전체의 접기 깊이를 다시 세지 않는다.
	class QTimer*       m_rstFoldTimer = nullptr;
};


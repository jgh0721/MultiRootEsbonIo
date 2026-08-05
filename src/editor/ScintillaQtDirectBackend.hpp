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
	void setFirstVisibleLine(int line);
	int linesOnScreen() const;

	// ── 뷰포트 비율 기준 스크롤 ──
	// "화면 세로 ratio 위치에 있는 줄" 과 "그 줄을 ratio 위치로 보내기".
	// 줄바꿈/코드 접기가 있어도 맞도록 visible line 좌표계를 쓴다.
	int lineAtViewportRatio(double ratio) const;
	void scrollLineToViewportRatio(int line, double ratio);
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
	void setKeywordsForLexer(const QString& lexerKey);
	void handleStyleNeeded(int endPosition);

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
};


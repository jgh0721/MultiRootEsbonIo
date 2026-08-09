#include "stdafx.h"
#include "ScintillaQtDirectBackend.hpp"

#include "core/solThemeManager.hpp"
#include "ScintillaEditorSettings.hpp"
#include "TextLexerRegistry.hpp"
#include "ScintillaDocument.hpp"

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QFile>
#include <QFontMetrics>
#include <QTextStream>
#include <QWidget>

#include <ScintillaEditBase.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>

namespace {

using MessageInt = unsigned int;
constexpr sptr_t kInvalidPosition = static_cast<sptr_t>(-1);
constexpr int kBraceHighlightIndicatorId = 24;
constexpr int kBraceBadLightIndicatorId = 25;

MessageInt sciMessage(int value)
{
	return static_cast<MessageInt>(value);
}

/// QColor -> Scintilla 색상 (0x00BBGGRR).
sptr_t sciColour(const QColor& color)
{
	return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
}

int changeHistoryFlagsForMode(const ScintillaEditorSettings::ChangeHistoryMode mode)
{
	switch (mode) {
		case ScintillaEditorSettings::ChangeHistoryMarkers:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS;
		case ScintillaEditorSettings::ChangeHistoryIndicators:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_INDICATORS;
		case ScintillaEditorSettings::ChangeHistoryBoth:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS | SC_CHANGE_HISTORY_INDICATORS;
		case ScintillaEditorSettings::ChangeHistoryOff:
		default:
			return SC_CHANGE_HISTORY_DISABLED;
	}
}

void writeLexerTrace(const QString& displayName, const QString& lexerKey, const bool applied)
{
	const QString tracePath = QString::fromLocal8Bit(qgetenv("MV_TEXT_LEXER_TRACE_FILE")).trimmed();
	if (tracePath.isEmpty())
		return;

	QFile traceFile(tracePath);
	if (!traceFile.open(QIODevice::Append | QIODevice::Text))
		return;

	QTextStream stream(&traceFile);
	stream << displayName << '|'
		   << lexerKey << '|'
		   << (applied ? QStringLiteral("applied") : QStringLiteral("failed"))
		   << Qt::endl;
}

bool isBraceCharacter(const sptr_t ch)
{
	switch (static_cast<char>(ch)) {
		case '(':
		case ')':
		case '[':
		case ']':
		case '{':
		case '}':
			return true;
		default:
			return false;
	}
}

void applyLineSpacingScale(ScintillaEditBase* editor, const QFont& font, const double scale)
{
	if (!editor)
		return;

	const double safeScale = qBound(1.0, scale, 3.0);
	const QFontMetrics metrics(font);
	const int baseLineHeight = qMax(1, metrics.lineSpacing());
	const int targetLineHeight = qMax(baseLineHeight, qRound(baseLineHeight * safeScale));
	const int extraPixels = qMax(0, targetLineHeight - baseLineHeight);
	const int extraAscent = extraPixels / 2;
	const int extraDescent = extraPixels - extraAscent;

	editor->send(sciMessage(SCI_SETEXTRAASCENT), extraAscent);
	editor->send(sciMessage(SCI_SETEXTRADESCENT), extraDescent);
}

sptr_t positionFromLineIndex(ScintillaEditBase* editor, const int line, const int index)
{
	if (!editor)
		return 0;

	const sptr_t lineCount = qMax<sptr_t>(1, editor->send(sciMessage(SCI_GETLINECOUNT)));
	const sptr_t safeLine = qBound<sptr_t>(0, static_cast<sptr_t>(line), lineCount - 1);
	const sptr_t linePosition = editor->send(sciMessage(SCI_POSITIONFROMLINE), safeLine);
	const sptr_t safeIndex = qMax<sptr_t>(0, static_cast<sptr_t>(index));
	const sptr_t documentPosition = editor->send(sciMessage(SCI_POSITIONRELATIVECODEUNITS), linePosition, safeIndex);
	const sptr_t lineEnd = editor->send(sciMessage(SCI_GETLINEENDPOSITION), safeLine);
	return qBound<sptr_t>(linePosition, documentPosition, lineEnd);
}

void positionToLineIndex(ScintillaEditBase* editor, const sptr_t position, int& line, int& index)
{
	if (!editor) {
		line = 0;
		index = 0;
		return;
	}

	const sptr_t safePosition = qBound<sptr_t>(0, position, editor->send(sciMessage(SCI_GETLENGTH)));
	line = static_cast<int>(editor->send(sciMessage(SCI_LINEFROMPOSITION), safePosition));
	const sptr_t linePosition = editor->send(sciMessage(SCI_POSITIONFROMLINE), line);
	index = static_cast<int>(editor->send(sciMessage(SCI_COUNTCODEUNITS), linePosition, safePosition));
}

} // namespace

ScintillaQtDirectBackend::ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent)
	: QObject(parent)
	, m_editor(new ScintillaEditBase(editorParent))
{
	m_editorFont = m_editor->font();

	// 리사이즈 시 깜빡임 방지: 시스템 배경 지움 비활성화
	m_editor->setAttribute(Qt::WA_OpaquePaintEvent);
	m_editor->setAttribute(Qt::WA_NoSystemBackground);

	// Windows DirectWrite 렌더링: 안티앨리어싱 격자 아티팩트 방지
	m_editor->send(sciMessage(SCI_SETTECHNOLOGY), 4 );
	m_editor->send(sciMessage(SCI_SETBUFFEREDDRAW), 0);
	m_editor->send(sciMessage(SCI_SETFONTQUALITY),
		static_cast<sptr_t>(Scintilla::FontQuality::QualityAntialiased));
	m_editor->send(sciMessage(SCI_SETIDLESTYLING), SC_IDLESTYLING_ALL);

	// 멀티 페이즈 드로잉: 배경을 연속된 영역으로 그려 글자 간 갭 방지
	m_editor->send(sciMessage(SCI_SETPHASESDRAW), SC_PHASES_MULTIPLE);
	// 레이아웃 캐시: 문서 전체 캐시로 서브픽셀 위치 일관성 유지
	m_editor->send(sciMessage(SCI_SETLAYOUTCACHE), SC_CACHE_PAGE);
	// 선택 레이어: 텍스트 아래 반투명 배경으로 그려 원래 lexer 글자가 계속 보이게 한다.
	m_editor->send(sciMessage(SCI_SETSELECTIONLAYER), SC_LAYER_UNDER_TEXT);
	// 사각형 선택 활성화
	m_editor->send(sciMessage(SCI_SETVIRTUALSPACEOPTIONS), SCVS_RECTANGULARSELECTION);

	// 제어문자/공백/EOL 기본 숨김
	m_editor->send(sciMessage(SCI_SETVIEWWS), SCWS_INVISIBLE);
	m_editor->send(sciMessage(SCI_SETVIEWEOL), 0);
	m_editor->send(sciMessage(SCI_SETCONTROLCHARSYMBOL), static_cast<sptr_t>(' '));

	// 마우스를 멈추면 SCN_DWELLSTART 가 온다 (:term: 호버 팝업).
	// 0 이면 dwell 통지 자체가 오지 않는다 (SC_TIME_FOREVER).
	m_editor->send(sciMessage(SCI_SETMOUSEDWELLTIME), 400);

	m_editor->send(sciMessage(SCI_SETCODEPAGE), Scintilla::CpUtf8);
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 0, static_cast<sptr_t>(Scintilla::MarginType::Number));
	configureBraceHighlightIndicators();
	configureCodeFolding(m_codeFoldingEnabled);
	updateLineNumberMargin();
	connect(m_editor, &QObject::destroyed, this, [this] {
		m_currentLexer = nullptr;
		m_editor = nullptr;
	});

	connect(m_editor, &ScintillaEditBase::savePointChanged,
			this, [this](bool dirty) {
				m_forcedModified = dirty;
				emit modificationChanged(dirty);
			});
	connect(m_editor, &ScintillaEditBase::notifyChange,
			this, [this] {
				syncLineCountState(true);
				emit textChanged();
			});
	connect(m_editor, &ScintillaEditBase::updateUi,
			this, [this](Scintilla::Update updated) {
				updateBraceHighlight();
				emitCursorAndSelectionState();
				if ((static_cast<int>(updated) & static_cast<int>(Scintilla::Update::VScroll)) != 0)
					emit viewportScrolled();
			});
	connect(m_editor, &ScintillaEditBase::styleNeeded,
			this, [this](Scintilla::Position position) {
				handleStyleNeeded(static_cast<int>(position));
				emit styleNeeded(static_cast<int>(position));
			});
	connect(m_editor, &ScintillaEditBase::charAdded,
			this, [this](int ch) {
				emit charAdded(ch);
			});
	connect(m_editor, &ScintillaEditBase::dwellStart,
			this, [this](int x, int y) {
				const int position = positionFromPoint(QPoint(x, y));
				if (position >= 0)
					emit dwellStarted(position, QPoint(x, y));
			});
	connect(m_editor, &ScintillaEditBase::dwellEnd,
			this, [this](int, int) {
				emit dwellEnded();
			});
	connect(m_editor, &ScintillaEditBase::marginClicked,
			this, [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
				if (!m_editor || !m_codeFoldingEnabled || margin != 1)
					return;
				const auto line = static_cast<sptr_t>(m_editor->send(sciMessage(SCI_LINEFROMPOSITION), position));
				m_editor->send(sciMessage(SCI_TOGGLEFOLD), line);
			});
	m_lastKnownLineCount = lineCount();
}

ScintillaQtDirectBackend::~ScintillaQtDirectBackend()
{
	clearLexer();
}

QWidget* ScintillaQtDirectBackend::widget() const
{
	return m_editor;
}

QAbstractScrollArea* ScintillaQtDirectBackend::scrollArea() const
{
	return m_editor;
}

void ScintillaQtDirectBackend::applySettings(const ScintillaEditorSettings& settings)
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_SETCODEPAGE), settings.useUtf8 ? Scintilla::CpUtf8 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 0, static_cast<sptr_t>(Scintilla::MarginType::Number));
	m_editor->send(sciMessage(SCI_SETINDENTATIONGUIDES),
		settings.showIndentationGuides ? static_cast<sptr_t>(settings.indentGuideStyle) : static_cast<sptr_t>(Scintilla::IndentView::None));
	m_editor->send(sciMessage(SCI_SETCARETLINEVISIBLE), settings.highlightCurrentLine ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETINDENT), settings.tabWidth);
	m_editor->send(sciMessage(SCI_SETTABWIDTH), settings.tabWidth);
	m_editor->send(sciMessage(SCI_SETUSETABS), settings.useTabs ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETTABINDENTS), settings.autoIndent ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETBACKSPACEUNINDENTS), settings.autoIndent ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETREADONLY), settings.readOnly ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETWRAPMODE),
		settings.wordWrap ? static_cast<sptr_t>(Scintilla::Wrap::Word)
		                  : static_cast<sptr_t>(Scintilla::Wrap::None));
	m_codeFoldingEnabled = settings.showCodeFolding;
	configureCodeFolding(m_codeFoldingEnabled);
	const int currentChangeHistoryFlags = static_cast<int>(m_editor->send(sciMessage(SCI_GETCHANGEHISTORY)));
	const int changeHistoryFlags = changeHistoryFlagsForMode(settings.changeHistoryMode);
	if (currentChangeHistoryFlags == SC_CHANGE_HISTORY_DISABLED && changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED) {
		// Scintilla는 changeHistory를 다시 켤 때 undo 이력이 남아 있으면 새 히스토리 버퍼를 만들지 않는다.
		// OFF -> ON 전환 시 이후 편집분이라도 표시되도록 undo 버퍼를 비워 새 baseline으로 재시작한다.
		m_editor->send(sciMessage(SCI_EMPTYUNDOBUFFER));
	}
	m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), changeHistoryFlags);

	const bool historyMarkersEnabled = (changeHistoryFlags & SC_CHANGE_HISTORY_MARKERS) != 0;
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 2, SC_MARGIN_SYMBOL);
	m_editor->send(sciMessage(SCI_SETMARGINMASKN), 2, SC_MASK_HISTORY);
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 2, historyMarkersEnabled ? 4 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINSENSITIVEN), 2, 0);

	m_editor->send(sciMessage(SCI_SETVIEWWS),
		settings.showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
	m_editor->send(sciMessage(SCI_SETVIEWEOL), settings.showWhitespace ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETCONTROLCHARSYMBOL),
		settings.showWhitespace ? 0 : static_cast<sptr_t>(' '));
	m_editor->send(sciMessage(SCI_SETFONTQUALITY),
		static_cast<sptr_t>(settings.fontRendering));
	applyLineSpacingScale(m_editor, m_editorFont.resolve(m_editor->font()), settings.lineSpacingScale);
	m_lineNumberMarginDigits = settings.showLineNumbers ? settings.lineNumberMarginDigits : 0;
	m_braceMatchingEnabled = settings.braceMatching;
	m_highlightCurrentLine = settings.highlightCurrentLine;
	configureBraceHighlightIndicators();
	if (!m_braceMatchingEnabled)
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
	updateBraceHighlight();
	updateLineNumberMargin(m_lineNumberMarginDigits);

	// 설정 변경 후 전체 화면 갱신 (탭 폭 등 즉시 반영)
	m_editor->send(sciMessage(SCI_COLOURISE), 0, -1);
	m_editor->update();
}

void ScintillaQtDirectBackend::setText(const QString& text)
{
	if (!m_editor)
		return;

	const int changeHistoryFlags = static_cast<int>(m_editor->send(sciMessage(SCI_GETCHANGEHISTORY)));
	if (changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED)
		m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), SC_CHANGE_HISTORY_DISABLED);

	const QByteArray utf8 = text.toUtf8();
	m_editor->sends(sciMessage(SCI_SETTEXT), 0, utf8.constData());
	m_editor->send(sciMessage(SCI_EMPTYUNDOBUFFER));
	m_editor->send(sciMessage(SCI_SETSAVEPOINT));
	if (changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED)
		m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), changeHistoryFlags);
	syncLineCountState(false);
	m_forcedModified = false;
}

QString ScintillaQtDirectBackend::text() const
{
	if (!m_editor)
		return {};

	const sptr_t length = m_editor->send(sciMessage(SCI_GETTEXTLENGTH));
	QByteArray buffer(static_cast<int>(length) + 1, Qt::Uninitialized);
	m_editor->send(sciMessage(SCI_GETTEXT), length + 1, reinterpret_cast<sptr_t>(buffer.data()));
	return QString::fromUtf8(buffer.constData());
}

void ScintillaQtDirectBackend::clear()
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_CLEARALL));
	m_forcedModified = false;
	syncLineCountState(false);
	updateBraceHighlight();
}

void ScintillaQtDirectBackend::setModified(bool modified)
{
	if (!m_editor)
		return;

	if (!modified)
		m_editor->send(sciMessage(SCI_SETSAVEPOINT));
	m_forcedModified = modified;
	emit modificationChanged(isModified());
}

bool ScintillaQtDirectBackend::isModified() const
{
	if (!m_editor)
		return false;

	return m_forcedModified || (m_editor->send(sciMessage(SCI_GETMODIFY)) != 0);
}

int ScintillaQtDirectBackend::lineCount() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETLINECOUNT))) : 0;
}

void ScintillaQtDirectBackend::setReadOnly(bool readOnly)
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_SETREADONLY), readOnly ? 1 : 0);
}

bool ScintillaQtDirectBackend::isReadOnly() const
{
	return m_editor ? (m_editor->send(sciMessage(SCI_GETREADONLY)) != 0) : false;
}

bool ScintillaQtDirectBackend::hasSelectedText() const
{
	return m_editor ? (m_editor->send(sciMessage(SCI_GETSELECTIONEMPTY)) == 0) : false;
}

QString ScintillaQtDirectBackend::selectedText() const
{
	if (!m_editor || !hasSelectedText())
		return {};

	const sptr_t lengthWithTerminator = m_editor->send(sciMessage(SCI_GETSELTEXT));
	if (lengthWithTerminator <= 1)
		return {};

	QByteArray buffer(static_cast<int>(lengthWithTerminator), Qt::Uninitialized);
	m_editor->send(sciMessage(SCI_GETSELTEXT), 0, reinterpret_cast<sptr_t>(buffer.data()));
	return QString::fromUtf8(buffer.constData());
}

bool ScintillaQtDirectBackend::getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const
{
	if (!m_editor) {
		lineFrom = 0;
		indexFrom = 0;
		lineTo = 0;
		indexTo = 0;
		return false;
	}

	const sptr_t selectionStart = m_editor->send(sciMessage(SCI_GETSELECTIONSTART));
	const sptr_t selectionEnd = m_editor->send(sciMessage(SCI_GETSELECTIONEND));
	positionToLineIndex(m_editor, selectionStart, lineFrom, indexFrom);
	positionToLineIndex(m_editor, selectionEnd, lineTo, indexTo);
	return selectionStart != selectionEnd;
}

void ScintillaQtDirectBackend::setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo)
{
	if (!m_editor)
		return;

	const sptr_t start = positionFromLineIndex(m_editor, lineFrom, indexFrom);
	const sptr_t end = positionFromLineIndex(m_editor, lineTo, indexTo);
	m_editor->send(sciMessage(SCI_SETSEL), start, end);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

void ScintillaQtDirectBackend::setSelectionByPos(int startPos, int endPos)
{
	if (!m_editor) return;
	m_editor->send(sciMessage(SCI_SETSEL), startPos, endPos);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

void ScintillaQtDirectBackend::copy()
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_COPY));
}

void ScintillaQtDirectBackend::paste()
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_PASTE));
}

bool ScintillaQtDirectBackend::canPaste() const
{
	return m_editor && m_editor->send(sciMessage(SCI_CANPASTE)) != 0;
}

void ScintillaQtDirectBackend::setCursorPosition(int line, int index)
{
	if (!m_editor)
		return;

	const sptr_t maxLine = qMax<sptr_t>(0, static_cast<sptr_t>(lineCount() - 1));
	const sptr_t safeLine = qMax<sptr_t>(0, qMin<sptr_t>(static_cast<sptr_t>(line), maxLine));
	const sptr_t safeIndex = qMax<sptr_t>(0, static_cast<sptr_t>(index));
	const sptr_t position = positionFromLineIndex(m_editor,
		static_cast<int>(safeLine),
		static_cast<int>(safeIndex));
	m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), position);
	m_editor->send(sciMessage(SCI_SCROLLCARET));
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

bool ScintillaQtDirectBackend::findFirst(const QString& text,
									 bool regex,
									 bool caseSensitive,
									 bool wholeWords,
									 bool wrap,
									 bool forward)
{
	if (!m_editor || text.isEmpty())
		return false;

	int flags = 0;
	if (caseSensitive)
		flags |= SCFIND_MATCHCASE;
	if (wholeWords)
		flags |= SCFIND_WHOLEWORD;
	if (regex)
		flags |= SCFIND_CXX11REGEX;

	int lineFrom = 0;
	int indexFrom = 0;
	int lineTo = 0;
	int indexTo = 0;
	if (getSelectionRange(lineFrom, indexFrom, lineTo, indexTo))
		setSelectionRange(forward ? lineTo : lineFrom,
					  forward ? indexTo : indexFrom,
					  forward ? lineTo : lineFrom,
					  forward ? indexTo : indexFrom);
	else
		m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), m_editor->send(sciMessage(SCI_GETCURRENTPOS)));

	const QByteArray needle = text.toUtf8();
	m_editor->send(sciMessage(SCI_SETSEARCHFLAGS), flags);
	m_editor->send(sciMessage(SCI_SEARCHANCHOR));

	const MessageInt message = sciMessage(forward ? SCI_SEARCHNEXT : SCI_SEARCHPREV);
	if (m_editor->sends(message, flags, needle.constData()) != -1) {
		m_editor->send(sciMessage(SCI_SCROLLCARET));
		return true;
	}

	if (!wrap)
		return false;

	const sptr_t wrapPosition = forward ? 0 : m_editor->send(sciMessage(SCI_GETLENGTH));
	m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), wrapPosition);
	m_editor->send(sciMessage(SCI_SEARCHANCHOR));
	if (m_editor->sends(message, flags, needle.constData()) == -1)
		return false;

	m_editor->send(sciMessage(SCI_SCROLLCARET));
	return true;
}

void ScintillaQtDirectBackend::replace(const QString& replacement)
{
	if (!m_editor)
		return;

	const QByteArray utf8 = replacement.toUtf8();
	m_editor->sends(sciMessage(SCI_REPLACESEL), 0, utf8.constData());
}

int ScintillaQtDirectBackend::countMatches(const QString& text,
										   bool regex,
										   bool caseSensitive,
										   bool wholeWords)
{
	if (!m_editor || text.isEmpty())
		return 0;
	return countMatchesInRange(text, regex, caseSensitive, wholeWords,
							   0, static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH))));
}

int ScintillaQtDirectBackend::countMatchesInRange(const QString& text,
												  bool regex,
												  bool caseSensitive,
												  bool wholeWords,
												  int startPos,
												  int endPos)
{
	if (!m_editor || text.isEmpty() || startPos >= endPos)
		return 0;

	int flags = 0;
	if (caseSensitive) flags |= SCFIND_MATCHCASE;
	if (wholeWords)    flags |= SCFIND_WHOLEWORD;
	if (regex)         flags |= SCFIND_CXX11REGEX;

	const QByteArray needle = text.toUtf8();
	int count = 0;
	int searchStart = startPos;

	while (searchStart < endPos) {
		m_editor->send(sciMessage(SCI_SETTARGETSTART), searchStart);
		m_editor->send(sciMessage(SCI_SETTARGETEND), endPos);
		m_editor->send(sciMessage(SCI_SETSEARCHFLAGS), flags);
		const sptr_t pos = m_editor->sends(sciMessage(SCI_SEARCHINTARGET),
										   static_cast<sptr_t>(needle.size()),
										   needle.constData());
		if (pos < 0)
			break;
		++count;
		const int matchEnd = static_cast<int>(m_editor->send(sciMessage(SCI_GETTARGETEND)));
		searchStart = qMax(matchEnd, searchStart + 1);
	}
	return count;
}

void ScintillaQtDirectBackend::setIndicatorStyle(int indicatorId, int style, const QColor& color)
{
	if (!m_editor)
		return;
	m_editor->send(sciMessage(SCI_INDICSETSTYLE), indicatorId, style);
	const int sciColor = color.red() | (color.green() << 8) | (color.blue() << 16);
	m_editor->send(sciMessage(SCI_INDICSETFORE), indicatorId, sciColor);
	m_editor->send(sciMessage(SCI_INDICSETALPHA), indicatorId, color.alpha());
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), indicatorId, qMax(color.alpha(), 160));
}

void ScintillaQtDirectBackend::applyIndicator(int indicatorId, int startPos, int length)
{
	if (!m_editor || length <= 0)
		return;
	m_editor->send(sciMessage(SCI_SETINDICATORCURRENT), indicatorId);
	m_editor->send(sciMessage(SCI_INDICATORFILLRANGE), startPos, length);
}

void ScintillaQtDirectBackend::clearIndicator(int indicatorId, int startPos, int length)
{
	if (!m_editor || length <= 0)
		return;
	m_editor->send(sciMessage(SCI_SETINDICATORCURRENT), indicatorId);
	m_editor->send(sciMessage(SCI_INDICATORCLEARRANGE), startPos, length);
}

void ScintillaQtDirectBackend::clearAllIndicator(int indicatorId)
{
	if (!m_editor)
		return;
	const int docLen = static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH)));
	clearIndicator(indicatorId, 0, docLen);
}

int ScintillaQtDirectBackend::selectionStartPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONSTART)));
}

int ScintillaQtDirectBackend::selectionEndPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONEND)));
}

int ScintillaQtDirectBackend::documentLength() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH)));
}

int ScintillaQtDirectBackend::currentPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETCURRENTPOS)));
}

int ScintillaQtDirectBackend::matchStartPos() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONSTART)));
}

int ScintillaQtDirectBackend::matchEndPos() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONEND)));
}

void ScintillaQtDirectBackend::scrollRangeToView(int startPos, int endPos)
{
	if (!m_editor)
		return;

	const sptr_t start = qMax<sptr_t>(0, static_cast<sptr_t>(startPos));
	const sptr_t end = qMax(start, static_cast<sptr_t>(endPos));
	m_editor->send(sciMessage(SCI_SCROLLRANGE), start, end);
}

void ScintillaQtDirectBackend::setSearchTargetRange(int startPos, int endPos)
{
	if (!m_editor) return;
	m_editor->send(sciMessage(SCI_SETTARGETSTART), startPos);
	m_editor->send(sciMessage(SCI_SETTARGETEND), endPos);
}

int ScintillaQtDirectBackend::searchInTarget(const QString& text, int flags)
{
	if (!m_editor) return -1;
	m_editor->send(sciMessage(SCI_SETSEARCHFLAGS), flags);
	const QByteArray utf8 = text.toUtf8();
	return static_cast<int>(m_editor->sends(sciMessage(SCI_SEARCHINTARGET), utf8.size(), utf8.constData()));
}

int ScintillaQtDirectBackend::targetEnd() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETTARGETEND)));
}

void ScintillaQtDirectBackend::replaceInTarget(const QString& text)
{
	if (!m_editor) return;
	const QByteArray utf8 = text.toUtf8();
	m_editor->sends(sciMessage(SCI_REPLACETARGET), utf8.size(), utf8.constData());
}

void ScintillaQtDirectBackend::setEditorFont(const QFont& font)
{
	if (!m_editor)
		return;

	m_editorFont = font;
	m_editor->setFont(font);

	const QByteArray family = font.family().toUtf8();
	const int pointSize = qMax(1, font.pointSize() > 0 ? font.pointSize() : font.pointSizeF() > 0.0 ? qRound(font.pointSizeF()) : 10);
	m_editor->sends(sciMessage(SCI_STYLESETFONT), STYLE_DEFAULT, family.constData());
	m_editor->send(sciMessage(SCI_STYLESETSIZE), STYLE_DEFAULT, pointSize);
	m_editor->sends(sciMessage(SCI_STYLESETFONT), STYLE_LINENUMBER, family.constData());
	m_editor->send(sciMessage(SCI_STYLESETSIZE), STYLE_LINENUMBER, pointSize);
	m_editor->send(sciMessage(SCI_STYLECLEARALL));
	updateLineNumberMargin(m_lineNumberMarginDigits);
}

QFont ScintillaQtDirectBackend::editorFont() const
{
	return m_editorFont;
}

int ScintillaQtDirectBackend::firstVisibleLine() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETFIRSTVISIBLELINE))) : 0;
}

void ScintillaQtDirectBackend::setFirstVisibleLine(int line)
{
	if (!m_editor)
		return;

	const sptr_t target = qMax<sptr_t>(0, static_cast<sptr_t>(line));
	m_editor->send(sciMessage(SCI_SETFIRSTVISIBLELINE), target);
}

int ScintillaQtDirectBackend::linesOnScreen() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_LINESONSCREEN))) : 0;
}

double ScintillaQtDirectBackend::fractionalLineAtViewportRatio(const double ratio) const
{
	if (!m_editor)
		return 1.0;

	// SCI_POSITIONFROMPOINT 는 픽셀 좌표를 받으므로 줄바꿈/접기와 무관하게
	// "화면 이 높이에 실제로 보이는 문자" 를 돌려준다.
	const int height = qMax(1, m_editor->height());
	const sptr_t y = static_cast<sptr_t>(height * qBound(0.0, ratio, 1.0));
	const sptr_t position = m_editor->send(sciMessage(SCI_POSITIONFROMPOINT), 0, y);
	if (position < 0)
		return qMax(1, firstVisibleLine() + 1);

	const int docLine = lineFromPosition(static_cast<int>(position));

	// 이 줄이 화면에서 여러 행으로 접혀 있으면, 그 안 어디쯤인지까지 표현한다.
	const sptr_t lineTopY = m_editor->send(sciMessage(SCI_POINTYFROMPOSITION), 0,
										   positionFromLine(docLine));
	const int rowHeight = qMax(1, textHeight(docLine));
	const int wrapRows = qMax(1, static_cast<int>(m_editor->send(sciMessage(SCI_WRAPCOUNT), docLine)));
	const double fraction = qBound(0.0,
								   static_cast<double>(y - lineTopY) / (wrapRows * rowHeight),
								   0.999);

	return ( docLine + 1 ) + fraction;   // 밖에서는 1-based
}

void ScintillaQtDirectBackend::scrollFractionalLineToViewportRatio(const double fractionalLine,
																   const double ratio)
{
	if (!m_editor)
		return;

	const int totalLines = qMax(1, lineCount());
	const double clamped = qBound(1.0, fractionalLine, static_cast<double>(totalLines));
	const int docLine = qBound(0, static_cast<int>(clamped) - 1, totalLines - 1);
	const double fraction = qBound(0.0, clamped - static_cast<int>(clamped), 0.999);

	const int height = qMax(1, m_editor->height());
	const int rowHeight = qMax(1, textHeight(docLine));
	const int wrapRows = qMax(1, static_cast<int>(m_editor->send(sciMessage(SCI_WRAPCOUNT), docLine)));

	// 목표: 그 줄의 fraction 지점이 화면 height*ratio 에 오는 것.
	// 따라서 줄의 "윗변" 은 그보다 fraction*줄높이 만큼 위에 있어야 한다.
	const double desiredTopY = height * qBound(0.0, ratio, 1.0) - fraction * wrapRows * rowHeight;
	const sptr_t currentTopY = m_editor->send(sciMessage(SCI_POINTYFROMPOSITION), 0,
											  positionFromLine(docLine));

	// 현재 위치와의 차이를 화면 행 수로 환산해 스크롤한다. SCI_LINESCROLL 은
	// 화면 행(wrap 된 하위 행 포함) 단위라 줄바꿈이 있어도 정확하다.
	const int deltaRows = static_cast<int>( qRound( ( currentTopY - desiredTopY ) / rowHeight ) );
	if (deltaRows != 0)
		m_editor->send(sciMessage(SCI_LINESCROLL), 0, deltaRows);
}

// ── 위치 변환 ──────────────────────────────────────────────

int ScintillaQtDirectBackend::positionFromLine(int line) const
{
	if (!m_editor)
		return 0;

	const sptr_t safeLine = qBound(sptr_t(0), static_cast<sptr_t>(line), static_cast<sptr_t>(qMax(0, lineCount() - 1)));
	return static_cast<int>(m_editor->send(sciMessage(SCI_POSITIONFROMLINE), safeLine));
}

int ScintillaQtDirectBackend::lineEndPosition(int line) const
{
	if (!m_editor)
		return 0;

	const sptr_t safeLine = qBound(sptr_t(0), static_cast<sptr_t>(line), static_cast<sptr_t>(qMax(0, lineCount() - 1)));
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETLINEENDPOSITION), safeLine));
}

int ScintillaQtDirectBackend::lineFromPosition(int position) const
{
	if (!m_editor)
		return 0;

	const sptr_t safePosition = qBound(sptr_t(0), static_cast<sptr_t>(position), static_cast<sptr_t>(documentLength()));
	return static_cast<int>(m_editor->send(sciMessage(SCI_LINEFROMPOSITION), safePosition));
}

int ScintillaQtDirectBackend::columnFromPosition(int position) const
{
	if (!m_editor)
		return 0;

	const sptr_t safePosition = qBound(sptr_t(0), static_cast<sptr_t>(position), static_cast<sptr_t>(documentLength()));
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETCOLUMN), safePosition));
}

int ScintillaQtDirectBackend::positionFromLineColumn(int line, int column) const
{
	if (!m_editor)
		return 0;

	return static_cast<int>(positionFromLineIndex(m_editor, line, column));
}

QByteArray ScintillaQtDirectBackend::textRangeUtf8(int startPos, int endPos) const
{
	if (!m_editor)
		return {};

	const sptr_t length = documentLength();
	const sptr_t start = qBound(sptr_t(0), static_cast<sptr_t>(startPos), length);
	const sptr_t end = qBound(start, static_cast<sptr_t>(endPos), length);
	if (end <= start)
		return {};

	QByteArray buffer(static_cast<qsizetype>(end - start) + 1, Qt::Uninitialized);
	Sci_TextRangeFull range{};
	range.chrg.cpMin = start;
	range.chrg.cpMax = end;
	range.lpstrText = buffer.data();
	m_editor->send(sciMessage(SCI_GETTEXTRANGEFULL), 0, reinterpret_cast<sptr_t>(&range));
	buffer.resize(static_cast<qsizetype>(end - start));
	return buffer;
}

QString ScintillaQtDirectBackend::lineText(int line) const
{
	if (!m_editor || line < 0 || line >= lineCount())
		return {};

	const sptr_t length = m_editor->send(sciMessage(SCI_LINELENGTH), line);
	if (length <= 0)
		return {};

	QByteArray buffer(static_cast<qsizetype>(length) + 1, Qt::Uninitialized);
	m_editor->send(sciMessage(SCI_GETLINE), line, reinterpret_cast<sptr_t>(buffer.data()));
	buffer.resize(static_cast<qsizetype>(length));
	return QString::fromUtf8(buffer);
}

QPoint ScintillaQtDirectBackend::pointFromPosition(int position) const
{
	if (!m_editor)
		return {};

	const sptr_t safePosition = qBound(sptr_t(0), static_cast<sptr_t>(position), static_cast<sptr_t>(documentLength()));
	const int x = static_cast<int>(m_editor->send(sciMessage(SCI_POINTXFROMPOSITION), 0, safePosition));
	const int y = static_cast<int>(m_editor->send(sciMessage(SCI_POINTYFROMPOSITION), 0, safePosition));
	return { x, y };
}

int ScintillaQtDirectBackend::positionFromPoint(const QPoint& viewportPos) const
{
	if (!m_editor)
		return -1;

	// CLOSE 계열은 글자 위가 아니면 -1 을 돌려준다. 빈 여백에서 호버 팝업이
	// 엉뚱한 위치의 텍스트를 집어 오는 것을 막아 준다.
	return static_cast<int>(m_editor->send(sciMessage(SCI_POSITIONFROMPOINTCLOSE),
										   viewportPos.x(), viewportPos.y()));
}

// ── 컨테이너 렉싱 지원 ─────────────────────────────────────

int ScintillaQtDirectBackend::endStyled() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETENDSTYLED))) : 0;
}

void ScintillaQtDirectBackend::startStyling(int position)
{
	if (!m_editor)
		return;

	const sptr_t safePosition = qBound(sptr_t(0), static_cast<sptr_t>(position), static_cast<sptr_t>(documentLength()));
	m_editor->send(sciMessage(SCI_STARTSTYLING), safePosition);
}

void ScintillaQtDirectBackend::setStylingEx(const QByteArray& styleBytes)
{
	if (!m_editor || styleBytes.isEmpty())
		return;

	m_editor->send(sciMessage(SCI_SETSTYLINGEX),
		static_cast<uptr_t>(styleBytes.size()),
		reinterpret_cast<sptr_t>(styleBytes.constData()));
}

void ScintillaQtDirectBackend::colouriseAll()
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_COLOURISE), 0, -1);
}

void ScintillaQtDirectBackend::setStyleForeground(int style, const QColor& color)
{
	if (m_editor && color.isValid())
		m_editor->send(sciMessage(SCI_STYLESETFORE), style, sciColour(color));
}

void ScintillaQtDirectBackend::setStyleBackground(int style, const QColor& color)
{
	if (m_editor && color.isValid())
		m_editor->send(sciMessage(SCI_STYLESETBACK), style, sciColour(color));
}

void ScintillaQtDirectBackend::setStyleBold(int style, bool bold)
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_STYLESETBOLD), style, bold ? 1 : 0);
}

void ScintillaQtDirectBackend::setStyleItalic(int style, bool italic)
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_STYLESETITALIC), style, italic ? 1 : 0);
}

void ScintillaQtDirectBackend::setStyleUnderline(int style, bool underline)
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_STYLESETUNDERLINE), style, underline ? 1 : 0);
}

void ScintillaQtDirectBackend::restoreViewState(int caretPosition, int firstVisibleLine)
{
	if (!m_editor)
		return;

	const sptr_t documentLength = m_editor->send(sciMessage(SCI_GETLENGTH));
	const sptr_t safeCaret = qBound<sptr_t>(0, static_cast<sptr_t>(caretPosition), documentLength);
	m_editor->send(sciMessage(SCI_GOTOPOS), safeCaret);

	const int currentFirstVisibleLine = static_cast<int>(m_editor->send(sciMessage(SCI_GETFIRSTVISIBLELINE)));
	m_editor->send(sciMessage(SCI_LINESCROLL), 0, qMax(0, firstVisibleLine) - currentFirstVisibleLine);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

int ScintillaQtDirectBackend::textHeight(int line) const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_TEXTHEIGHT), line)) : 0;
}

int ScintillaQtDirectBackend::leftMarginWidth() const
{
	if (!m_editor)
		return 0;
	// 모든 마진(줄 번호, 기호 등) 폭 합산
	int total = 0;
	for (int i = 0; i < 5; ++i)
		total += static_cast<int>(m_editor->send(sciMessage(SCI_GETMARGINWIDTHN), i));
	// 텍스트 영역 왼쪽 패딩 (텍스트 시작 위치까지의 추가 간격)
	total += static_cast<int>(m_editor->send(sciMessage(SCI_GETMARGINLEFT)));
	return total;
}

int ScintillaQtDirectBackend::changeHistoryFlags() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETCHANGEHISTORY))) : 0;
}

void ScintillaQtDirectBackend::setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting)
{
	if (!m_editor)
		return;

	sptr_t eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::CrLf);
	if (lineEnding == ScintillaDocument::LF)
		eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::Lf);
	else if (lineEnding == ScintillaDocument::CR)
		eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::Cr);

	m_editor->send(sciMessage(SCI_SETEOLMODE), eolMode);
	if (convertExisting)
		m_editor->send(sciMessage(SCI_CONVERTEOLS), eolMode);
}

bool ScintillaQtDirectBackend::applyLanguage(const QString& displayName)
{
	if (!m_editor)
		return false;

	clearLexer();

	const QString lexerKey = TextLexerRegistry::instance().lexerKeyForDisplayName(displayName);

	// reStructuredText: Lexilla 에 렉서가 없으므로 컨테이너 렉싱을 쓴다.
	// ILexer 를 비운 채 SCI_COLOURISE 를 보내면 Scintilla 가 SCN_STYLENEEDED 로
	// 스타일을 요청하고, handleStyleNeeded() 가 직접 칠한다.
	if (lexerKey.compare(QStringLiteral("rst-container"), Qt::CaseInsensitive) == 0) {
		m_rstLexer = std::make_unique<mrst::rst::RstContainerLexer>();
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
		m_editor->send(sciMessage(SCI_SETPROPERTY),
			reinterpret_cast<uptr_t>("fold"),
			reinterpret_cast<sptr_t>("0"));
		m_currentLexerKey = lexerKey;
		applySyntaxStyles(m_darkTheme);
		m_editor->send(sciMessage(SCI_COLOURISE), 0, -1);
		writeLexerTrace(displayName, lexerKey, true);
		return true;
	}

	if (lexerKey.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
		// ILexer 를 비우면 Scintilla 는 컨테이너 렉싱 모드가 되지만, 여기서는
		// SCI_COLOURISE 를 보내지 않으므로 styleNeeded 도 발생하지 않아 무채색이 된다.
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
		m_editor->send(sciMessage(SCI_SETPROPERTY),
			reinterpret_cast<uptr_t>("fold"),
			reinterpret_cast<sptr_t>("0"));
		writeLexerTrace(displayName, lexerKey, true);
		return true;
	}

	const QByteArray lexerName = lexerKey.toUtf8();

	// Scintilla 5 에는 SCI_SETLEXER / SCI_SETLEXERLANGUAGE 가 없다. 렉서 지정 수단은
	// Lexilla 의 CreateLexer() 로 만든 ILexer5 를 SCI_SETILEXER 로 넘기는 것뿐이다.
	m_currentLexer = CreateLexer(lexerName.constData());
	if (!m_currentLexer) {
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
		writeLexerTrace(displayName, lexerKey, false);
		return false;
	}

	m_editor->send(sciMessage(SCI_SETILEXER), 0, reinterpret_cast<sptr_t>(m_currentLexer));

	m_editor->send(sciMessage(SCI_SETPROPERTY),
		reinterpret_cast<uptr_t>("fold"),
		m_codeFoldingEnabled ? reinterpret_cast<sptr_t>("1") : reinterpret_cast<sptr_t>("0"));
	m_editor->send(sciMessage(SCI_SETPROPERTY),
		reinterpret_cast<uptr_t>("fold.compact"),
		reinterpret_cast<sptr_t>("1"));
	configureCodeFolding(m_codeFoldingEnabled);

	m_currentLexerKey = lexerKey;
	setKeywordsForLexer(lexerKey);
	applySyntaxStyles(m_darkTheme);

	m_editor->send(sciMessage(SCI_COLOURISE), 0, -1);
	// SCI_GETLEXER 는 ILexer 의 GetIdentifier() 를 그대로 돌려주므로 렉서마다 값이
	// 제각각이다. 적용 여부는 CreateLexer() 결과로 판단하는 편이 정확하다.
	const bool applied = m_currentLexer != nullptr;
	writeLexerTrace(displayName, lexerKey, applied);
	return applied;
}

void ScintillaQtDirectBackend::configureCodeFolding(bool enabled)
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 1, SC_MARGIN_SYMBOL);
	m_editor->send(sciMessage(SCI_SETMARGINMASKN), 1, SC_MASK_FOLDERS);
	m_editor->send(sciMessage(SCI_SETMARGINSENSITIVEN), 1, enabled ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 1, enabled ? 16 : 0);
	m_editor->send(sciMessage(SCI_SETFOLDFLAGS), SC_FOLDFLAG_LINEAFTER_CONTRACTED);

	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

	const auto markerFore = static_cast<sptr_t>(0xFFFFFF);
	const auto markerBack = static_cast<sptr_t>(0x808080);
	for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
		m_editor->send(sciMessage(SCI_MARKERSETFORE), marker, markerFore);
		m_editor->send(sciMessage(SCI_MARKERSETBACK), marker, markerBack);
	}
}

void ScintillaQtDirectBackend::updateLineNumberMargin(int minimumDigits)
{
	if (!m_editor)
		return;

	if (minimumDigits <= 0) {
		m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 0, 0);
		return;
	}

	const int digits = qMax(minimumDigits, QString::number(qMax(1, lineCount())).size());
	const QFontMetrics metrics(m_editorFont.resolve(m_editor->font()));
	const int width = metrics.horizontalAdvance(QString(digits, QLatin1Char('9'))) + 12;
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 0, width);
}

void ScintillaQtDirectBackend::configureBraceHighlightIndicators()
{
	if (!m_editor)
		return;

	auto colourToSci = [](const QColor& color) -> sptr_t {
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};

	Q_UNUSED(m_darkTheme);
	const QColor matchColor = ThemeManager::instance().color(QStringLiteral("text.braceMatch"));
	const QColor badColor = ThemeManager::instance().color(QStringLiteral("text.braceMismatch"));

	// 괄호 매칭은 글자를 덮지 않도록 채움 없는 outline indicator로 표시한다.
	m_editor->send(sciMessage(SCI_INDICSETSTYLE), kBraceHighlightIndicatorId, INDIC_STRAIGHTBOX);
	m_editor->send(sciMessage(SCI_INDICSETFORE), kBraceHighlightIndicatorId, colourToSci(matchColor));
	m_editor->send(sciMessage(SCI_INDICSETALPHA), kBraceHighlightIndicatorId, 0);
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), kBraceHighlightIndicatorId, qMax(matchColor.alpha(), 220));

	m_editor->send(sciMessage(SCI_INDICSETSTYLE), kBraceBadLightIndicatorId, INDIC_SQUIGGLE);
	m_editor->send(sciMessage(SCI_INDICSETFORE), kBraceBadLightIndicatorId, colourToSci(badColor));
	m_editor->send(sciMessage(SCI_INDICSETALPHA), kBraceBadLightIndicatorId, badColor.alpha());
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), kBraceBadLightIndicatorId, badColor.alpha());

	m_editor->send(sciMessage(SCI_BRACEHIGHLIGHTINDICATOR), 1, kBraceHighlightIndicatorId);
	m_editor->send(sciMessage(SCI_BRACEBADLIGHTINDICATOR), 1, kBraceBadLightIndicatorId);
}

void ScintillaQtDirectBackend::syncLineCountState(bool emitSignal)
{
	if (!m_editor)
		return;

	const int currentLineCount = qMax(1, lineCount());
	const bool lineCountChanged = currentLineCount != m_lastKnownLineCount;
	m_lastKnownLineCount = currentLineCount;
	updateLineNumberMargin(m_lineNumberMarginDigits);
	if (emitSignal && lineCountChanged)
		emit linesChanged();
}

void ScintillaQtDirectBackend::emitCursorAndSelectionState()
{
	if (!m_editor)
		return;

	const sptr_t position = m_editor->send(sciMessage(SCI_GETCURRENTPOS));
	int line = 0;
	int column = 0;
	positionToLineIndex(m_editor, position, line, column);
	emit cursorPositionChanged(line, column);
	emit selectionChanged();
}

void ScintillaQtDirectBackend::updateBraceHighlight()
{
	if (!m_editor)
		return;

	if (!m_braceMatchingEnabled) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
		return;
	}

	const sptr_t currentPos = m_editor->send(sciMessage(SCI_GETCURRENTPOS));
	sptr_t bracePos = currentPos;
	sptr_t braceChar = m_editor->send(sciMessage(SCI_GETCHARAT), bracePos);
	if (!isBraceCharacter(braceChar) && currentPos > 0) {
		bracePos = currentPos - 1;
		braceChar = m_editor->send(sciMessage(SCI_GETCHARAT), bracePos);
	}

	if (!isBraceCharacter(braceChar)) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
		return;
	}

	sptr_t matchPos = m_editor->send(sciMessage(SCI_BRACEMATCH), bracePos, 0);
	if (matchPos == kInvalidPosition)
		matchPos = m_editor->send(sciMessage(SCI_BRACEMATCHNEXT), bracePos, currentPos);
	if (matchPos == kInvalidPosition) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), bracePos);
		return;
	}

	m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
	m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), bracePos, matchPos);
}

void ScintillaQtDirectBackend::clearLexer()
{
	if (!m_currentLexer)
		return;

	// SCI_SETILEXER에 넘긴 lexer의 수명은 Scintilla가 관리한다.
	// 백엔드는 편집기가 살아 있는 동안에만 분리 요청을 보내고,
	// 편집기가 먼저 파괴된 경우에는 포인터만 무효화한다.
	if (m_editor)
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
	m_currentLexer = nullptr;
	m_currentLexerKey.clear();
	m_rstLexer.reset();
}

mrst::rst::RstMetadataCache* ScintillaQtDirectBackend::rstMetadataCache() const
{
	return m_rstLexer ? &m_rstLexer->metadataCache() : nullptr;
}

void ScintillaQtDirectBackend::restyleDocument()
{
	if (!m_editor)
		return;

	// SCI_STARTSTYLING(0) 으로 endStyled 를 되돌려야 문서 전체가 다시 요청된다.
	m_editor->send(sciMessage(SCI_STARTSTYLING), 0);
	m_editor->send(sciMessage(SCI_COLOURISE), 0, -1);
}

void ScintillaQtDirectBackend::handleStyleNeeded(int endPosition)
{
	if (!m_editor || !m_rstLexer)
		return;

	const int documentEnd = documentLength();
	const int startPosition = endStyled();
	if (endPosition <= startPosition || startPosition >= documentEnd)
		return;

	const int totalLines = qMax(1, lineCount());
	const int firstLine = lineFromPosition(startPosition);
	const int lastLine = qMin(lineFromPosition(qMin(endPosition, documentEnd)), totalLines - 1);

	// 렉서는 제목/구분선 판정을 위해 앞뒤 한 줄을 문맥으로 요구한다.
	// 문맥까지 포함해 렉싱한 뒤, 실제로 칠할 구간만 잘라 쓴다.
	const int contextFirstLine = qMax(0, firstLine - 1);
	const int contextLastLine = qMin(totalLines - 1, lastLine + 1);

	const int contextStart = positionFromLine(contextFirstLine);
	const int contextEnd = (contextLastLine + 1 < totalLines)
		? positionFromLine(contextLastLine + 1)
		: documentEnd;

	const QByteArray chunk = textRangeUtf8(contextStart, contextEnd);
	if (chunk.isEmpty())
		return;

	const std::vector<unsigned char> styles =
		m_rstLexer->styleBytes(std::string(chunk.constData(), static_cast<std::size_t>(chunk.size())));

	const int paintStart = positionFromLine(firstLine);
	const int paintEnd = (lastLine + 1 < totalLines) ? positionFromLine(lastLine + 1) : documentEnd;
	const int offset = paintStart - contextStart;
	const int length = paintEnd - paintStart;
	if (offset < 0 || length <= 0 || static_cast<std::size_t>(offset + length) > styles.size())
		return;

	QByteArray styleBytes(length, Qt::Uninitialized);
	std::memcpy(styleBytes.data(), styles.data() + offset, static_cast<std::size_t>(length));

	startStyling(paintStart);
	setStylingEx(styleBytes);
}

void ScintillaQtDirectBackend::applyRstSyntaxStyles()
{
	if (!m_editor)
		return;

	using namespace mrst::rst;
	auto& theme = ThemeManager::instance();
	const auto rstColour = [&theme](const char* token) {
		return theme.color(QStringLiteral("text.lexer.rst.%1").arg(QLatin1String(token)));
	};

	setStyleForeground(STYLE_TITLE, rstColour("title"));
	setStyleBold(STYLE_TITLE, true);
	setStyleForeground(STYLE_TRANSITION, rstColour("transition"));
	setStyleForeground(STYLE_COMMENT, rstColour("comment"));
	setStyleItalic(STYLE_COMMENT, true);
	setStyleForeground(STYLE_EXPLICIT_MARKUP, rstColour("explicitMarkup"));

	setStyleForeground(STYLE_DIRECTIVE_VALID, rstColour("directiveValid"));
	setStyleBold(STYLE_DIRECTIVE_VALID, true);
	setStyleForeground(STYLE_DIRECTIVE_INVALID, rstColour("directiveInvalid"));
	setStyleBold(STYLE_DIRECTIVE_INVALID, true);
	setStyleForeground(STYLE_DIRECTIVE_UNKNOWN, rstColour("directiveUnknown"));
	setStyleBold(STYLE_DIRECTIVE_UNKNOWN, true);

	setStyleForeground(STYLE_ROLE_VALID, rstColour("roleValid"));
	setStyleForeground(STYLE_ROLE_INVALID, rstColour("roleInvalid"));
	setStyleForeground(STYLE_ROLE_UNKNOWN, rstColour("roleUnknown"));

	setStyleForeground(STYLE_LITERAL, rstColour("literal"));
	setStyleForeground(STYLE_INLINE_LITERAL, rstColour("inlineLiteral"));
	setStyleForeground(STYLE_EMPHASIS, rstColour("emphasis"));
	setStyleItalic(STYLE_EMPHASIS, true);
	setStyleForeground(STYLE_STRONG, rstColour("strong"));
	setStyleBold(STYLE_STRONG, true);
	setStyleForeground(STYLE_INTERPRETED, rstColour("interpreted"));
	setStyleForeground(STYLE_HYPERLINK, rstColour("hyperlink"));
	setStyleUnderline(STYLE_HYPERLINK, true);
	setStyleForeground(STYLE_SUBSTITUTION, rstColour("substitution"));
	setStyleForeground(STYLE_FIELD_NAME, rstColour("fieldName"));
	setStyleBold(STYLE_FIELD_NAME, true);
}

void ScintillaQtDirectBackend::applyThemeColors(bool dark)
{
	if (!m_editor)
		return;

	Q_UNUSED(dark);
	auto colourToSci = [](const QColor& color) -> sptr_t {
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};

	auto& theme = ThemeManager::instance();
	const QColor bg = theme.color(QStringLiteral("text.background"));
	const QColor fg = theme.color(QStringLiteral("text.foreground"));
	const QColor marginBg = theme.color(QStringLiteral("text.marginBackground"));
	const QColor marginFg = theme.color(QStringLiteral("text.marginForeground"));
	const QColor selection = theme.color(QStringLiteral("text.selection"));
	const QColor selectionForeground = theme.color(QStringLiteral("text.selectionForeground"));
	const QColor caretLine = theme.color(QStringLiteral("text.currentLine"));
	const QColor caret = theme.color(QStringLiteral("text.caret"));
	const QColor foldMarker = theme.color(QStringLiteral("text.foldMarker"));
	const QColor indentGuide = theme.color(QStringLiteral("text.indentGuide"));

	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_DEFAULT, colourToSci(fg));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_DEFAULT, colourToSci(bg));
	m_editor->send(sciMessage(SCI_STYLECLEARALL));
	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_INDENTGUIDE, colourToSci(indentGuide));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_INDENTGUIDE, colourToSci(bg));

	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_LINENUMBER, colourToSci(marginFg));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_LINENUMBER, colourToSci(marginBg));

	m_editor->send(sciMessage(SCI_SETCARETFORE), colourToSci(caret));
	m_editor->send(sciMessage(SCI_SETCARETLINEVISIBLEALWAYS), 1);
	m_editor->send(sciMessage(SCI_SETCARETLINEBACK), colourToSci(caretLine));

	m_editor->send(sciMessage(SCI_SETSELBACK), 1, colourToSci(selection));
	m_editor->send(sciMessage(SCI_SETSELECTIONLAYER), SC_LAYER_UNDER_TEXT);
	m_editor->send(sciMessage(SCI_SETSELALPHA), selection.alpha() > 0 ? selection.alpha() : 112);
	m_editor->send(sciMessage(SCI_SETSELFORE), 0, colourToSci(selectionForeground));

	m_editor->send(sciMessage(SCI_SETFOLDMARGINCOLOUR), 1, colourToSci(bg));
	m_editor->send(sciMessage(SCI_SETFOLDMARGINHICOLOUR), 1, colourToSci(bg));
	for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
		m_editor->send(sciMessage(SCI_MARKERSETFORE), marker, colourToSci(bg));
		m_editor->send(sciMessage(SCI_MARKERSETBACK), marker, colourToSci(foldMarker));
	}

	// 현재 언어의 구문 강조 색상 적용
	m_darkTheme = ThemeManager::instance().currentTheme() == ThemeManager::Dark;
	configureBraceHighlightIndicators();
	applySyntaxStyles(m_darkTheme);

	// SCI_STYLECLEARALL 이 컨테이너 스타일까지 초기화하므로 다시 칠해야 한다.
	if (m_rstLexer)
		restyleDocument();

	m_editor->update();
}

void ScintillaQtDirectBackend::applySyntaxStyles(bool dark)
{
	if (!m_editor || m_currentLexerKey.isEmpty())
		return;

	// reST 는 Lexilla 스타일 번호 체계를 쓰지 않으므로 별도 경로.
	if (m_currentLexerKey == QStringLiteral("rst-container")) {
		applyRstSyntaxStyles();
		return;
	}

	auto colourToSci = [](unsigned int r, unsigned int g, unsigned int b) -> sptr_t {
		return static_cast<sptr_t>(r | (g << 8) | (b << 16));
	};

	// C/C++ 계열 렉서 (cpp, javascript, java, cs 등이 모두 cpp lexer를 사용)
	if (m_currentLexerKey == QStringLiteral("cpp")) {
		if (dark) {
			// VS Code Dark+ 기반 색상
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x6A, 0x99, 0x55)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0x6A, 0x99, 0x55)); // comment line doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 16, colourToSci(0x4E, 0xC9, 0xB0)); // keyword2 (types)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xC5, 0x86, 0xC0)); // preprocessor
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 11, colourToSci(0x9C, 0xDC, 0xFE)); // identifier
			m_editor->send(sciMessage(SCI_STYLESETFORE), 19, colourToSci(0x4E, 0xC9, 0xB0)); // globalclass
		} else {
			// VS Code Light+ 기반 색상
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x00, 0x80, 0x00)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0x00, 0x80, 0x00)); // comment line doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 16, colourToSci(0x26, 0x7F, 0x99)); // keyword2 (types)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xAF, 0x00, 0xDB)); // preprocessor
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 11, colourToSci(0x1e, 0x1e, 0x1e)); // identifier
			m_editor->send(sciMessage(SCI_STYLESETFORE), 19, colourToSci(0x26, 0x7F, 0x99)); // globalclass
		}
	}
	// Python 렉서
	else if (m_currentLexerKey == QStringLiteral("python")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 12, colourToSci(0x6A, 0x99, 0x55)); // comment block
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // triple
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // triple double
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x4E, 0xC9, 0xB0)); // keyword2 (builtins)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x4E, 0xC9, 0xB0)); // classname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xDC, 0xDC, 0xAA)); // defname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0xD7, 0xBA, 0x7D)); // decorator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 12, colourToSci(0x00, 0x80, 0x00)); // comment block
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // triple
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // triple double
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x26, 0x7F, 0x99)); // keyword2 (builtins)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x26, 0x7F, 0x99)); // classname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x79, 0x5E, 0x26)); // defname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0xAF, 0x00, 0xDB)); // decorator
		}
	}
	// JSON 렉서
	else if (m_currentLexerKey == QStringLiteral("json")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // line comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // block comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x9C, 0xDC, 0xFE)); // propertyname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xCE, 0x91, 0x78)); // stringeol
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x56, 0x9C, 0xD6)); // keyword (true/false/null)
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 8,  1);
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // line comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // block comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x00, 0x16, 0x80)); // propertyname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xA3, 0x15, 0x15)); // stringeol
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 8,  1);
		}
	}
	// XML/HTML 렉서
	else if (m_currentLexerKey == QStringLiteral("xml") || m_currentLexerKey == QStringLiteral("hypertext")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x56, 0x9C, 0xD6)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x9C, 0xDC, 0xFE)); // attribute
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 17, colourToSci(0xD4, 0xD4, 0xD4)); // CDATA
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x80, 0x00, 0x00)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xFF, 0x00, 0x00)); // attribute
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x00, 0x00, 0xFF)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x00, 0x00, 0xFF)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 17, colourToSci(0x1e, 0x1e, 0x1e)); // CDATA
		}
	}
	// CSS 렉서
	else if (m_currentLexerKey == QStringLiteral("css")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0xD7, 0xBA, 0x7D)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x9C, 0xDC, 0xFE)); // class
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // pseudoclass
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x9C, 0xDC, 0xFE)); // property (attribute)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 13, colourToSci(0xCE, 0x91, 0x78)); // value (string)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0xB5, 0xCE, 0xA8)); // value (number)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x6A, 0x99, 0x55)); // comment
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x80, 0x00, 0x00)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x80, 0x00, 0x00)); // class
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x80, 0x00, 0x00)); // pseudoclass
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0xFF, 0x00, 0x00)); // property
			m_editor->send(sciMessage(SCI_STYLESETFORE), 13, colourToSci(0x00, 0x00, 0xFF)); // value (string)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x09, 0x86, 0x58)); // value (number)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x00, 0x80, 0x00)); // comment
		}
	}
	// Bash 렉서
	else if (m_currentLexerKey == QStringLiteral("bash")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 4,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xCE, 0x91, 0x78)); // string (single)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string (double)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x9C, 0xDC, 0xFE)); // variable (scalar)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xD4, 0xD4, 0xD4)); // operator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 4,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xA3, 0x15, 0x15)); // string (single)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string (double)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x00, 0x16, 0x80)); // variable
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x1e, 0x1e, 0x1e)); // operator
		}
	}
	// SQL 렉서
	else if (m_currentLexerKey == QStringLiteral("sql")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x6A, 0x99, 0x55)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x00, 0x80, 0x00)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
		}
	}

	// 사용자 테마 토큰을 최종 override로 적용한다. 기본값은 Monokai 팔레트다.
	auto colorKeyForToken = [this](const QString& genericKey) {
		const QString prefix = QStringLiteral("text.lexer.");
		if (!genericKey.startsWith(prefix))
			return genericKey;

		const QString token = genericKey.mid(prefix.size());
		const QString lexerSpecific = QStringLiteral("text.lexer.%1.%2").arg(m_currentLexerKey, token);
		if (ThemeManager::instance().hasColorOverride(lexerSpecific))
			return lexerSpecific;

		if (m_currentLexerKey == QStringLiteral("hypertext")) {
			const QString xmlSpecific = QStringLiteral("text.lexer.xml.%1").arg(token);
			if (ThemeManager::instance().hasColorOverride(xmlSpecific))
				return xmlSpecific;
		}

		return genericKey;
	};
	auto themeColor = [&colorKeyForToken](const QString& key) -> sptr_t {
		const QColor color = ThemeManager::instance().color(colorKeyForToken(key));
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};
	auto setStyleFore = [this, &themeColor](int style, const QString& key) {
		m_editor->send(sciMessage(SCI_STYLESETFORE), style, themeColor(key));
	};

	if (m_currentLexerKey == QStringLiteral("cpp")) {
		for (int style : {1, 2, 3, 15}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(4, QStringLiteral("text.lexer.number"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		setStyleFore(16, QStringLiteral("text.lexer.type"));
		setStyleFore(6, QStringLiteral("text.lexer.string"));
		setStyleFore(7, QStringLiteral("text.lexer.string"));
		setStyleFore(9, QStringLiteral("text.lexer.preprocessor"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
		setStyleFore(11, QStringLiteral("text.lexer.identifier"));
		setStyleFore(19, QStringLiteral("text.lexer.function"));
	} else if (m_currentLexerKey == QStringLiteral("python")) {
		for (int style : {1, 12}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(2, QStringLiteral("text.lexer.number"));
		for (int style : {3, 4, 6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		setStyleFore(14, QStringLiteral("text.lexer.type"));
		setStyleFore(8, QStringLiteral("text.lexer.function"));
		setStyleFore(9, QStringLiteral("text.lexer.function"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
		setStyleFore(15, QStringLiteral("text.lexer.preprocessor"));
	} else if (m_currentLexerKey == QStringLiteral("json")) {
		for (int style : {1, 2}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(3, QStringLiteral("text.lexer.number"));
		for (int style : {4, 5}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(6, QStringLiteral("text.lexer.function"));
		setStyleFore(7, QStringLiteral("text.lexer.operator"));
		setStyleFore(8, QStringLiteral("text.lexer.keyword"));
	} else if (m_currentLexerKey == QStringLiteral("xml") || m_currentLexerKey == QStringLiteral("hypertext")) {
		setStyleFore(1, QStringLiteral("text.lexer.keyword"));
		setStyleFore(3, QStringLiteral("text.lexer.preprocessor"));
		for (int style : {6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(9, QStringLiteral("text.lexer.comment"));
		setStyleFore(17, QStringLiteral("text.lexer.identifier"));
	} else if (m_currentLexerKey == QStringLiteral("css")) {
		for (int style : {1, 2, 5}) setStyleFore(style, QStringLiteral("text.lexer.keyword"));
		setStyleFore(8, QStringLiteral("text.lexer.preprocessor"));
		setStyleFore(13, QStringLiteral("text.lexer.string"));
		setStyleFore(14, QStringLiteral("text.lexer.number"));
		setStyleFore(9, QStringLiteral("text.lexer.comment"));
	} else if (m_currentLexerKey == QStringLiteral("bash")) {
		setStyleFore(2, QStringLiteral("text.lexer.comment"));
		setStyleFore(3, QStringLiteral("text.lexer.number"));
		setStyleFore(4, QStringLiteral("text.lexer.keyword"));
		for (int style : {5, 6}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(8, QStringLiteral("text.lexer.variable"));
		setStyleFore(7, QStringLiteral("text.lexer.operator"));
	} else if (m_currentLexerKey == QStringLiteral("sql")) {
		for (int style : {1, 2, 3}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(4, QStringLiteral("text.lexer.number"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		for (int style : {6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
	}
}

void ScintillaQtDirectBackend::setKeywordsForLexer(const QString& lexerKey)
{
	if (!m_editor)
		return;

	if (lexerKey == QStringLiteral("cpp")) {
		// C/C++ keywords (keyword list 0)
		static const char cppKeywords[] =
			"alignas alignof and and_eq asm auto bitand bitor bool break case catch "
			"char char8_t char16_t char32_t class compl concept const consteval constexpr "
			"constinit const_cast continue co_await co_return co_yield decltype default "
			"delete do double dynamic_cast else enum explicit export extern false final "
			"float for friend goto if import inline int long module mutable namespace new "
			"noexcept not not_eq nullptr operator or or_eq override private protected public "
			"register reinterpret_cast requires return short signed sizeof static "
			"static_assert static_cast struct switch template this thread_local throw true "
			"try typedef typeid typename union unsigned using virtual void volatile "
			"wchar_t while xor xor_eq "
			// Common C keywords
			"_Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn "
			"_Static_assert _Thread_local restrict "
			// Common extensions
			"__attribute__ __declspec __cdecl __stdcall __fastcall __thiscall "
			"int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
			"size_t ssize_t ptrdiff_t intptr_t uintptr_t";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(cppKeywords));

		// C++ types/secondary keywords (keyword list 1)
		static const char cppTypes[] =
			"string wstring vector map unordered_map set unordered_set list deque queue "
			"stack array pair tuple optional variant any shared_ptr unique_ptr weak_ptr "
			"function thread mutex atomic future promise "
			"QString QStringList QWidget QObject QList QVector QHash QMap QSet "
			"QPointer QSharedPointer QScopedPointer QByteArray QVariant";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 1,
			reinterpret_cast<sptr_t>(cppTypes));
	}
	else if (lexerKey == QStringLiteral("python")) {
		static const char pythonKeywords[] =
			"False None True and as assert async await break class continue def del "
			"elif else except finally for from global if import in is lambda nonlocal "
			"not or pass raise return try while with yield";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(pythonKeywords));

		// Python builtins (keyword list 1)
		static const char pythonBuiltins[] =
			"abs all any bin bool bytearray bytes callable chr classmethod compile complex "
			"delattr dict dir divmod enumerate eval exec filter float format frozenset "
			"getattr globals hasattr hash help hex id input int isinstance issubclass iter "
			"len list locals map max memoryview min next object oct open ord pow print "
			"property range repr reversed round set setattr slice sorted staticmethod str "
			"sum super tuple type vars zip __import__";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 1,
			reinterpret_cast<sptr_t>(pythonBuiltins));
	}
	else if (lexerKey == QStringLiteral("sql")) {
		static const char sqlKeywords[] =
			"select from where insert into values update set delete create table alter drop "
			"index view trigger procedure function begin end if else then case when null "
			"not and or in exists between like is join inner outer left right cross on "
			"as order by group having distinct count sum avg min max cast union all top "
			"limit offset declare varchar int integer char text float double date datetime "
			"primary key foreign references default constraint unique check grant revoke "
			"commit rollback transaction exec execute";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(sqlKeywords));
	}
	else if (lexerKey == QStringLiteral("bash")) {
		static const char bashKeywords[] =
			"if then else elif fi case esac for while until do done in function select "
			"time coproc break continue return exit shift export readonly declare local "
			"typeset unset eval exec source alias unalias trap set shopt "
			"echo printf read cd pwd ls cat grep find test true false";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(bashKeywords));
	}
}


#include "stdafx.h"
#include "FindReplaceWidget.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QStyle>
#include <QApplication>
#include <QPainter>
#include <QPixmap>

namespace {

/// 검색 옵션 토글.
///
/// 예전에는 QLineEdit::addAction(TrailingPosition) 으로 입력칸 **안**에 넣었는데,
/// Qlementine 스타일은 QLineEdit 내부 아이콘 버튼의 Move 이벤트를 가로채 인덱스와
/// 무관하게 전부 오른쪽 끝 한 자리에 겹쳐 놓는다(LineEditButtonEventFilter).
/// 그래서 토글들을 입력칸 밖으로 꺼내고, 입력칸 안에는 내장 clear 버튼 하나만 남긴다.
QToolButton* makeOptionButton(QWidget* parent, const QString& text, const QString& toolTip)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    // 아이콘이 없으므로 글자를 그리게 명시한다 (기본값은 IconOnly).
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setFixedHeight(24);
    button->setMinimumWidth(26);
    return button;
}

} // namespace

// ═══════════════════════════════════════════════════════════
// 생성자
// ═══════════════════════════════════════════════════════════

FindReplaceWidget::FindReplaceWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    setReplaceMode(false);
    // 이벤트 필터 설치 (ESC, F3, Enter 처리)
    m_searchEdit->installEventFilter(this);
    m_replaceEdit->installEventFilter(this);
}

// ═══════════════════════════════════════════════════════════
// UI 구성
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 2, 4, 2);
    mainLayout->setSpacing(2);

    // ── 상단 행: 검색 ──────────────────────────────────────
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(2);

    // > 토글 버튼 (치환 행 접기/펼치기)
    m_toggleBtn = new QToolButton(this);
    m_toggleBtn->setArrowType(Qt::RightArrow);
    m_toggleBtn->setFixedSize(20, 20);
    m_toggleBtn->setToolTip(tr("바꾸기 표시/숨기기"));
    m_toggleBtn->setCheckable(true);
    connect(m_toggleBtn, &QToolButton::toggled, this, [this](bool checked) {
        setReplaceMode(checked);
    });
    topRow->addWidget(m_toggleBtn);

    // 검색 입력 필드 — 내부 위젯은 내장 clear 버튼 하나뿐이다.
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setMaximumWidth(280);
    m_searchEdit->setPlaceholderText(tr("검색"));
    m_searchEdit->setClearButtonEnabled(true);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        emit findTextChanged(text);
    });

    topRow->addWidget(m_searchEdit);

    // T1: 대/소문자 구분
    m_caseBtn = makeOptionButton(this, QStringLiteral("Aa"), tr("대/소문자 구분"));
    connect(m_caseBtn, &QToolButton::toggled, this, [this] {
        updatePlaceholderText();
        emit optionsChanged();
    });
    topRow->addWidget(m_caseBtn);

    // T2: 단어 일치
    m_wordBtn = makeOptionButton(this, QStringLiteral("W"), tr("단어 일치"));
    connect(m_wordBtn, &QToolButton::toggled, this, [this] {
        updatePlaceholderText();
        emit optionsChanged();
    });
    topRow->addWidget(m_wordBtn);

    // 결과 개수 라벨 — 내용이 없으면 자리를 차지하지 않는다.
    m_countLabel = new QLabel(this);
    m_countLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    m_countLabel->setVisible(false);
    topRow->addWidget(m_countLabel);

    m_excludedCountLabel = new QLabel(this);
    m_excludedCountLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    m_excludedCountLabel->setVisible(false);
    topRow->addWidget(m_excludedCountLabel);

    // Btn1: 이전 항목
    m_prevBtn = new QToolButton(this);
    m_prevBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_prevBtn->setToolTip(tr("이전 찾기 (Shift+F3)"));
    m_prevBtn->setAutoRaise(true);
    connect(m_prevBtn, &QToolButton::clicked, this, &FindReplaceWidget::findPrev);
    topRow->addWidget(m_prevBtn);

    // Btn2: 다음 항목
    m_nextBtn = new QToolButton(this);
    m_nextBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_nextBtn->setToolTip(tr("다음 찾기 (F3)"));
    m_nextBtn->setAutoRaise(true);
    connect(m_nextBtn, &QToolButton::clicked, this, &FindReplaceWidget::findNext);
    topRow->addWidget(m_nextBtn);

    // Btn3: 필터 버튼 (메뉴)
    m_filterBtn = new QToolButton(this);
    // 글리프는 번역 대상이 아니다. tr() 로 감싸면 번역자 화면에 뜻 없는 기호가
    // 뜨고, 실수로 글자로 바뀌면 버튼 모양이 깨진다.
    m_filterBtn->setText(QStringLiteral("⫶"));
    m_filterBtn->setToolTip(tr("필터"));
    m_filterBtn->setAutoRaise(true);
    m_filterBtn->setPopupMode(QToolButton::InstantPopup);
    m_filterMenu = new QMenu(this);
    m_searchInSelectionAction = m_filterMenu->addAction(tr("선택 영역에서 검색"));
    m_searchInSelectionAction->setCheckable(true);
    m_searchInSelectionAction->setEnabled(false); // 선택 없으면 비활성
    connect(m_searchInSelectionAction, &QAction::toggled, this, [this] {
        emit optionsChanged();
    });
    m_filterBtn->setMenu(m_filterMenu);
    topRow->addWidget(m_filterBtn);

    // Btn4: 메뉴 버튼
    m_menuBtn = new QToolButton(this);
    m_menuBtn->setText(QStringLiteral("☰"));
    m_menuBtn->setToolTip(tr("옵션"));
    m_menuBtn->setAutoRaise(true);
    m_menuBtn->setPopupMode(QToolButton::InstantPopup);
    m_optionsMenu = new QMenu(this);
    m_autoScrollAction = m_optionsMenu->addAction(tr("입력 결과로 스크롤"));
    m_autoScrollAction->setCheckable(true);
    m_autoScrollAction->setChecked(true);
    connect(m_autoScrollAction, &QAction::toggled, this, [this] {
        emit optionsChanged();
    });
    m_menuBtn->setMenu(m_optionsMenu);
    topRow->addWidget(m_menuBtn);

    // X 닫기 버튼
    m_closeBtn = new QToolButton(this);
    m_closeBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    m_closeBtn->setToolTip(tr("닫기 (ESC)"));
    m_closeBtn->setAutoRaise(true);
    connect(m_closeBtn, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });
    topRow->addWidget(m_closeBtn);

    // 남는 폭은 전부 여기로 — 라벨이나 버튼이 늘어나 위치가 흔들리지 않는다.
    topRow->addStretch(1);

    mainLayout->addLayout(topRow);

    // ── 하단 행: 치환 ──────────────────────────────────────
    m_replaceRow = new QWidget(this);
    auto* bottomRow = new QHBoxLayout(m_replaceRow);
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(2);

    // 토글 버튼과 같은 너비의 스페이서
    bottomRow->addSpacing(22);

    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setMaximumWidth(280);
    m_replaceEdit->setPlaceholderText(tr("바꿀 내용"));
    bottomRow->addWidget(m_replaceEdit);

    // Btn5: 바꾸기
    m_replaceBtn = new QPushButton(tr("바꾸기(&R)"), m_replaceRow);
    m_replaceBtn->setToolTip(tr("현재 강조된 항목 바꾸기"));
    connect(m_replaceBtn, &QPushButton::clicked, this, &FindReplaceWidget::replaceRequested);
    bottomRow->addWidget(m_replaceBtn);

    // Btn6: 모두 바꾸기
    m_replaceAllBtn = new QPushButton(tr("모두 바꾸기(&A)"), m_replaceRow);
    m_replaceAllBtn->setToolTip(tr("검색된 모든 항목 바꾸기"));
    connect(m_replaceAllBtn, &QPushButton::clicked, this, &FindReplaceWidget::replaceAllRequested);
    bottomRow->addWidget(m_replaceAllBtn);

    // Btn7: 제외
    m_excludeBtn = new QPushButton(tr("제외(&E)"), m_replaceRow);
    m_excludeBtn->setToolTip(tr("현재 항목을 바꾸기 대상에서 제외"));
    connect(m_excludeBtn, &QPushButton::clicked, this, &FindReplaceWidget::excludeRequested);
    bottomRow->addWidget(m_excludeBtn);

    mainLayout->addWidget(m_replaceRow);

    // 불투명 배경 + 하단 구분선
    setAutoFillBackground(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, false);
}

// ═══════════════════════════════════════════════════════════
// 공개 API
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::setReplaceMode(bool replace)
{
    m_replaceMode = replace;
    m_replaceRow->setVisible(replace);
    m_toggleBtn->blockSignals(true);
    m_toggleBtn->setChecked(replace);
    m_toggleBtn->setArrowType(replace ? Qt::DownArrow : Qt::RightArrow);
    m_toggleBtn->blockSignals(false);

    if (QLayout* lay = layout())
        lay->invalidate();
    m_replaceRow->updateGeometry();
    updateGeometry();
    adjustSize();
    if (QWidget* p = parentWidget()) {
        if (QLayout* parentLayout = p->layout())
            parentLayout->invalidate();
        p->updateGeometry();
    }
}

bool FindReplaceWidget::isReplaceMode() const
{
    return m_replaceMode;
}

QString FindReplaceWidget::searchText() const
{
    return m_searchEdit ? m_searchEdit->text() : QString();
}

QString FindReplaceWidget::replaceText() const
{
    return m_replaceEdit ? m_replaceEdit->text() : QString();
}

bool FindReplaceWidget::isCaseSensitive() const
{
    return m_caseBtn && m_caseBtn->isChecked();
}

bool FindReplaceWidget::isWholeWord() const
{
    return m_wordBtn && m_wordBtn->isChecked();
}

bool FindReplaceWidget::isSearchInSelection() const
{
    return m_searchInSelectionAction && m_searchInSelectionAction->isChecked();
}

bool FindReplaceWidget::isAutoScrollToFirst() const
{
    return m_autoScrollAction && m_autoScrollAction->isChecked();
}

void FindReplaceWidget::setSearchInSelectionEnabled(bool enabled)
{
    if (m_searchInSelectionAction) {
        m_searchInSelectionAction->setEnabled(enabled);
        if (!enabled && m_searchInSelectionAction->isChecked())
            m_searchInSelectionAction->setChecked(false);
    }
}

void FindReplaceWidget::setSearchInSelectionChecked(bool checked)
{
    if (m_searchInSelectionAction && m_searchInSelectionAction->isEnabled()) {
        m_searchInSelectionAction->setChecked(checked);
    }
}

void FindReplaceWidget::setMatchCount(int count)
{
    if (!m_countLabel)
        return;

    // 언어가 바뀌면 라벨을 다시 만들어야 하는데, 표시된 텍스트에서 개수를
    // 되읽을 방법이 없다. 마지막 값을 들고 있는다.
    m_lastMatchCount = count;
    const bool show = count >= 0 && !(m_searchEdit && m_searchEdit->text().isEmpty());
    m_countLabel->setText(show ? tr("결과 %n 개", nullptr, count) : QString());
    m_countLabel->setVisible(show);
}

void FindReplaceWidget::setExcludedCount(int count)
{
    if (!m_excludedCountLabel)
        return;

    m_lastExcludedCount = count;
    const bool show = count > 0 && !(m_searchEdit && m_searchEdit->text().isEmpty());
    m_excludedCountLabel->setText(show ? tr("제외 %n 개", nullptr, count) : QString());
    m_excludedCountLabel->setVisible(show);
}

void FindReplaceWidget::focusSearchField()
{
    if (m_searchEdit) {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    }
}

void FindReplaceWidget::setSelectionRange(int startPos, int endPos)
{
    m_selectionStart = startPos;
    m_selectionEnd = endPos;
}

// ═══════════════════════════════════════════════════════════
// PlaceholderText 업데이트
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::updatePlaceholderText()
{
    if (!m_searchEdit)
        return;

    // 번역된 낱말을 번역된 구분자로 잇지 않는다. 조합이 네 가지뿐이라 문장을
    // 그대로 넷 만든다 — 일본어는 나열 구분자가 "、" 이고 언어에 따라 괄호가
    // 자연스러운데, tr(", ") 로는 어느 쪽도 맞출 수 없다.
    const bool matchCase  = m_caseBtn && m_caseBtn->isChecked();
    const bool wholeWords = m_wordBtn && m_wordBtn->isChecked();

    if (matchCase && wholeWords)
        m_searchEdit->setPlaceholderText(tr("검색 (대/소문자 구분, 단어 일치)"));
    else if (matchCase)
        m_searchEdit->setPlaceholderText(tr("검색 (대/소문자 구분)"));
    else if (wholeWords)
        m_searchEdit->setPlaceholderText(tr("검색 (단어 일치)"));
    else
        m_searchEdit->setPlaceholderText(tr("검색"));
}

void FindReplaceWidget::changeEvent(QEvent* event)
{
    // LanguageChange 는 최상위 위젯에서 자식 트리로 재귀 전달되므로, 편집기
    // 안쪽 깊이 있는 이 위젯도 그대로 받는다.
    if (event && event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void FindReplaceWidget::retranslateUi()
{
    m_toggleBtn->setToolTip(tr("바꾸기 표시/숨기기"));
    m_caseBtn->setToolTip(tr("대/소문자 구분"));
    m_wordBtn->setToolTip(tr("단어 일치"));
    m_prevBtn->setToolTip(tr("이전 찾기 (Shift+F3)"));
    m_nextBtn->setToolTip(tr("다음 찾기 (F3)"));
    m_filterBtn->setToolTip(tr("필터"));
    m_menuBtn->setToolTip(tr("옵션"));
    m_closeBtn->setToolTip(tr("닫기 (ESC)"));

    m_searchInSelectionAction->setText(tr("선택 영역에서 검색"));
    m_autoScrollAction->setText(tr("입력 결과로 스크롤"));

    m_replaceEdit->setPlaceholderText(tr("바꿀 내용"));
    m_replaceBtn->setText(tr("바꾸기(&R)"));
    m_replaceBtn->setToolTip(tr("현재 강조된 항목 바꾸기"));
    m_replaceAllBtn->setText(tr("모두 바꾸기(&A)"));
    m_replaceAllBtn->setToolTip(tr("검색된 모든 항목 바꾸기"));
    m_excludeBtn->setText(tr("제외(&E)"));
    m_excludeBtn->setToolTip(tr("현재 항목을 바꾸기 대상에서 제외"));

    // 검색 placeholder 와 개수 라벨은 현재 값에서 다시 만든다.
    updatePlaceholderText();
    setMatchCount(m_lastMatchCount);
    setExcludedCount(m_lastExcludedCount);
}

// ═══════════════════════════════════════════════════════════
// paintEvent (불투명 배경 보장)
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    // 하단 구분선
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

// ═══════════════════════════════════════════════════════════
// 이벤트 필터 (ESC, F3, Shift+F3)
// ═══════════════════════════════════════════════════════════

bool FindReplaceWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            hide();
            emit closed();
            return true;
        }
        if (keyEvent->key() == Qt::Key_F3) {
            if (keyEvent->modifiers() & Qt::ShiftModifier)
                emit findPrev();
            else
                emit findNext();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (obj == m_searchEdit) {
                if (keyEvent->modifiers() & Qt::ShiftModifier)
                    emit findPrev();
                else
                    emit findNext();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}








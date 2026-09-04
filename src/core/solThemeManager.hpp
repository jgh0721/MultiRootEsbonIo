#pragma once

#include <QObject>
#include <QWidget>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QString>

/// 색상 편집 범위 식별자.
///
/// 화면에 보이는 범위 이름(scopeLabel)과 코드가 비교에 쓰는 값을 갈라 놓는다.
/// tr() 로 만든 문자열을 그대로 식별자로 쓰면, 언어를 바꾸는 순간 콤보에 남아
/// 있던 userData 와 새로 만든 표의 값이 달라져 그 범위가 통째로 걸러진다.
/// 같은 원문이 ThemeManager 와 QSettingsDialog 두 컨텍스트에 존재하므로
/// 번역자가 한쪽만 다르게 옮겨도 같은 일이 벌어진다.
namespace ThemeScopeIds
{
    inline constexpr auto                   kCommon            = "common";
    inline constexpr auto                   kPdf               = "pdf";
    inline constexpr auto                   kImage             = "image";
    inline constexpr auto                   kMarkdown          = "markdown";
    inline constexpr auto                   kText              = "text";
    inline constexpr auto                   kTextLexer         = "text.lexer";
    inline constexpr auto                   kTextLexerDetail   = "text.lexer.detail";
    inline constexpr auto                   kTextLexerRst      = "text.lexer.rst";
    inline constexpr auto                   kTextLexerMarkdown = "text.lexer.markdown";
}

/// 전역 테마 관리자 (싱글톤)
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    enum Theme { Light, Dark };
    Q_ENUM(Theme)

    /// 화면 글꼴의 적용 범위. UI 는 QApplication 기본 글꼴이고, 나머지는
    /// 해당 패널의 항목 글꼴만 덮는다.
    enum class FontRole
    {
        UserInterface,
        Explorer,
        Outline,
        DiagnosticsAndLog,
    };
    Q_ENUM( FontRole )

    static constexpr int                kMinimumFontPointSize = 6;
    static constexpr int                kMaximumFontPointSize = 72;

    struct ColorEntry {
        QString key;
        QString label;
        /// ThemeScopeIds 중 하나. 번역하지 않는다 — 비교는 전부 이 값으로 한다.
        QString groupId;
        /// 화면에 보이는 범위 이름. editableColorEntries() 가 scopeLabel() 로 채운다.
        QString group;
    };

    static ThemeManager&                instance();

    Theme                               currentTheme() const;
    /// 테마를 바꾸고 설정에 남긴다. 어느 경로("보기 > 테마 전환", 설정
    /// 대화상자)로 바꾸든 다음 실행에 그대로 이어진다.
    void                                setTheme( Theme theme );
    /// 설정에 남아 있는 테마. 값이 없거나 망가졌으면 기본(다크).
    /// 위젯을 만들기 전인 main() 에서 쓰라고 정적 함수로 둔다.
    static Theme                        savedTheme();

    QColor                              color( const QString& key ) const;
    bool                                hasColor( const QString& key ) const;
    bool                                hasColorOverride( const QString& key ) const;
    void                                setColorOverride( const QString& key, const QColor& color );
    void                                setColorOverrides( const QHash< QString, QColor >& colors );
    void                                resetColorOverrides( Theme theme );
    QHash< QString, QColor >            colorOverrides( Theme theme ) const;
    QHash< QString, QColor >            effectiveColors( Theme theme ) const;

    static QList<ColorEntry>            editableColorEntries();
    /// ThemeScopeIds 값을 화면에 보일 이름으로 바꾼다. 범위 이름을 만드는 곳은
    /// 여기 하나뿐이라, 설정 대화상자가 만드는 항목과 문자열이 어긋날 수 없다.
    static QString                      scopeLabel( const QString& groupId );
    static QHash<QString, QColor>       defaultColors( Theme theme );
    static QString                      themeName( Theme theme );

    /// 저장된 화면 글꼴. 패널별 값이 없으면 UI 글꼴을 사용한다.
    static QFont                        configuredFont( FontRole role );
    /// 화면 글꼴을 설정에 저장한다. 실제 위젯 적용은 applyToApplication() 과
    /// MainWindow::applyConfiguredFonts() 가 담당한다.
    static void                         setConfiguredFont( FontRole role, const QFont& font );

    bool                                importThemeFile( const QString& filePath, QString* errorMessage = nullptr );
    bool                                exportThemeFile( const QString& filePath, QString* errorMessage = nullptr ) const;

    // 테마별 색상 조회
    QColor                              backgroundColor() const;
    QColor                              foregroundColor() const;
    QColor                              toolBarColor() const;
    QColor                              tabActiveColor() const;
    QColor                              tabInactiveColor() const;
    QColor                              canvasAreaColor() const;

    // 앱 전체 스타일시트 적용
    void                                applyToApplication();

signals:
    void                                themeChanged( Theme theme );

private:
    ThemeManager();
    void                                loadOverrides();
    void                                saveOverrides() const;

    // 기본 테마는 다크다. main() 이 저장된 설정으로 덮어쓴다.
    Theme                               m_theme = Dark;
    QHash<QString, QColor>              m_lightOverrides;
    QHash<QString, QColor>              m_darkOverrides;
};


#pragma once

#include <QObject>
#include <QWidget>
#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

/// 전역 테마 관리자 (싱글톤)
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    enum Theme { Light, Dark };
    Q_ENUM(Theme)

    struct ColorEntry {
        QString key;
        QString label;
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
    static QHash<QString, QColor>       defaultColors( Theme theme );
    static QString                      themeName( Theme theme );

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


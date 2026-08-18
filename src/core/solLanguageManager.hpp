#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTranslator>

/// 전역 언어 관리자 (싱글톤)
///
/// ThemeManager 와 짝을 이룬다 — main() 이 위젯을 만들기 전에 정적 함수로
/// 저장값을 읽고, 그 뒤의 변경은 인스턴스 하나를 통해서만 일어난다.
///
/// **enum 이 아니라 언어 코드 문자열을 쓴다.** Theme 은 라이트/다크가 각각
/// `:/themes/*.json` 과 1:1 로 묶인 "구조" 지만, 지원 언어는 .ts 를 하나 더
/// 놓으면 늘어나는 "데이터" 다. enum 이면 언어를 추가할 때마다 enum 값 / 저장된
/// 정수 / 콤보 매핑이 함께 움직여야 하고, ini 를 손으로 고친 사용자가 범위 밖
/// 정수를 넣었을 때 savedTheme() 처럼 clamp 하는 코드가 또 필요하다. 코드
/// 문자열은 QTranslator::load() 와 QLocale 이 그대로 받는 형식이라 변환 계층이
/// 사라지고, ini 에도 `language=ja` 로 남아 사람이 읽을 수 있다.
class LanguageManager : public QObject
{
    Q_OBJECT

public:
    /// "시스템 설정 따름" 센티널.
    ///
    /// 빈 문자열이 아니라 눈에 보이는 낱말을 쓴다. ini 는 사람이 열어 보는
    /// 파일이고, `language=` 만 남아 있으면 값을 지운 것인지 시스템을 따르라고
    /// 고른 것인지 구분할 수 없다.
    static constexpr auto               kFollowSystem = "system";

    /// 설정 콤보를 채우는 한 줄.
    struct Entry
    {
        QString                         code;          ///< kFollowSystem 또는 "ko" / "en" / "ja"
        QString                         displayName;   ///< 화면에 보이는 이름
    };

    static LanguageManager&             instance();

    /// 설정에 저장된 값. kFollowSystem 이거나 지원 언어 코드다.
    [[nodiscard]] QString               selectedLanguage() const;
    /// 지금 실제로 적용된 언어. selectedLanguage() 가 kFollowSystem 이면
    /// 시스템에서 유추한 결과가 들어 있다.
    [[nodiscard]] QString               effectiveLanguage() const;

    /// 언어를 바꾸고 설정에 남긴다.
    ///
    /// 번역기를 갈아 끼우면 Qt 가 QEvent::LanguageChange 를 모든 최상위 위젯에
    /// 뿌려 주므로, 이 함수를 부른 쪽이 화면을 따로 새로 그릴 필요는 없다.
    void                                setLanguage( const QString& code );

    /// 설정에 남아 있는 값. 위젯을 만들기 전인 main() 에서 쓰라고 정적 함수로
    /// 둔다. 모르는 코드가 적혀 있으면 kFollowSystem 으로 본다.
    [[nodiscard]] static QString        savedLanguage();

    /// main() 이 QApplication 을 만든 직후, 첫 위젯을 만들기 전에 한 번 부른다.
    static void                         installAtStartup();

    /// 설정 대화상자용. 첫 항목은 항상 kFollowSystem 이다.
    [[nodiscard]] static QList< Entry > availableLanguages();

    /// Windows 표시 언어에서 유추한 지원 언어 코드. 못 고르면 "en".
    [[nodiscard]] static QString        systemLanguage();

    /// 지원 언어 코드인가. kFollowSystem 은 여기 포함하지 않는다.
    [[nodiscard]] static bool           isSupported( const QString& code );

    /// 언어 코드를 그 언어의 이름으로. 모르는 코드는 그대로 돌려준다.
    [[nodiscard]] static QString        nativeName( const QString& code );

signals:
    /// 번역기 교체가 끝난 뒤에 나간다. LanguageChange 이벤트가 닿지 않는
    /// 곳(모델, 캐시, 정적 표를 미리 읽어 둔 자료구조)을 갱신할 때 쓴다.
    void                                languageChanged( const QString& code );

private:
    LanguageManager() = default;

    /// 번역기를 떼고 다시 붙인다. 순서가 중요해서 한 곳에만 둔다.
    void                                applyTranslator( const QString& code );

    /// **QTranslator 는 하나면 된다.** Qt 자신의 번역(qtbase)은 빌드 시점에
    /// lrelease 가 우리 .qm 안으로 합쳐 넣는다
    /// (CMakeLists.txt 의 `MERGE_QT_TRANSLATIONS`).
    ///
    /// installTranslator() 뒤에도 계속 살아 있어야 한다. main() 의
    /// `do { ... } while(false)` 블록 안에 스택 객체로 두면 블록을 벗어나는
    /// 순간 파괴되고, QCoreApplication 은 죽은 포인터를 들고 번역을 찾는다.
    QTranslator                         m_translator;
    QString                             m_effective;
};

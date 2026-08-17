#pragma once

#include <string>

/// 업데이터가 **직접 띄우는 창**의 문구.
///
/// **Qt 를 링크하지 않으므로 tr() 도 QTranslator 도 쓸 수 없다.** Qt6Core.dll 을
/// 로드하면 자기가 교체해야 하는 파일을 스스로 잠근다(CMakeLists.txt 참고).
/// 그래서 문자열을 소스에 박아 넣고 언어별로 골라 쓴다.
///
/// 여기 있는 것은 **사용자가 업데이터를 손으로 실행했을 때** 보이는 문구뿐이다
/// (인자 없이 실행 / --rollback). 앱이 실행할 때 업데이터는 창을 하나도 띄우지
/// 않으므로 앱이 언어를 넘겨 줄 이유가 없다 — 그래서 --lang 같은 인자를 만들지
/// 않고 Windows 표시 언어를 본다.
///
/// **로그(updater.log)와 실패 사유(result.ini)는 여기 없다.** 로그는 개발자용
/// 기록이라 번역하면 사용자가 보내 온 로그를 읽을 때마다 번역판을 맞춰 봐야
/// 하고, 실패 사유는 앱이 자기 언어로 문장을 만든다(FailureId 참고).
/// 그 경계가 이 헤더의 존재 이유다.
namespace mrst::updater
{
    enum class UiLang
    {
        Ko,
        En,
        Ja,
        Count
    };

    enum class Text
    {
        Title,             ///< MessageBox 캡션
        Usage,             ///< 인자 없이 실행했을 때의 안내
        RollbackOk,        ///< --rollback 성공
        RollbackFailed,    ///< --rollback 실패
        Count
    };

    /// GetUserDefaultUILanguage() 의 primary language id 로 고른다.
    ///
    /// GetUserDefaultLocaleName() 이 **아니다** — 그쪽은 "국가 또는 지역 > 형식"
    /// 설정이라, 한국에서 영어판 Windows 를 쓰면 ko-KR 이 나온다. 우리가 알고
    /// 싶은 것은 "메뉴가 무슨 말로 나오는가" 다.
    [[nodiscard]] UiLang         detectUiLanguage();

    [[nodiscard]] const wchar_t* text( Text id, UiLang lang );
}   // namespace mrst::updater

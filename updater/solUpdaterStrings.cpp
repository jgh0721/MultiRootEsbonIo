#include "solUpdaterStrings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mrst::updater
{

UiLang detectUiLanguage()
{
    switch( PRIMARYLANGID( ::GetUserDefaultUILanguage() ) )
    {
        case LANG_KOREAN:   return UiLang::Ko;
        case LANG_JAPANESE: return UiLang::Ja;
        default:            return UiLang::En;   // 앱과 같은 규칙: 그 외는 영어
    }
}

const wchar_t* text( const Text id, const UiLang lang )
{
    // [Text][UiLang] 순서로 둔다. 한 메시지의 세 언어가 나란히 붙어 있어야
    // 새 메시지를 넣을 때 한 언어만 빠뜨린 것이 눈에 띈다.
    static const wchar_t* const kTable[ static_cast< int >( Text::Count ) ]
                                      [ static_cast< int >( UiLang::Count ) ] = {
        /* Title */ {
            L"MultiRoot reST 업데이터",
            L"MultiRoot reST Updater",
            L"MultiRoot reST アップデーター",
        },
        /* Usage */ {
            L"이 프로그램은 MultiRoot reST 가 업데이트를 적용할 때 자동으로 실행됩니다.\r\n"
            L"\r\n"
            L"직접 되돌리려면:\r\n"
            L"  \"MultiRoot-reST Updater.exe\" --rollback --target <설치폴더> "
            L"--backup <설치폴더>\\.update\\backup --log <로그파일>",

            L"This program runs automatically when MultiRoot reST applies an update.\r\n"
            L"\r\n"
            L"To roll back manually:\r\n"
            L"  \"MultiRoot-reST Updater.exe\" --rollback --target <install folder> "
            L"--backup <install folder>\\.update\\backup --log <log file>",

            L"このプログラムは MultiRoot reST が更新を適用するときに自動的に実行されます。\r\n"
            L"\r\n"
            L"手動で元に戻すには:\r\n"
            L"  \"MultiRoot-reST Updater.exe\" --rollback --target <インストール先> "
            L"--backup <インストール先>\\.update\\backup --log <ログファイル>",
        },
        /* RollbackOk */ {
            L"이전 버전으로 되돌렸습니다.",
            L"Rolled back to the previous version.",
            L"以前のバージョンに戻しました。",
        },
        /* RollbackFailed */ {
            L"되돌리지 못했습니다. .update\\updater.log 를 확인해 주세요.",
            L"Roll back failed. Please check .update\\updater.log.",
            L"元に戻せませんでした。.update\\updater.log を確認してください。",
        },
    };
    return kTable[ static_cast< int >( id ) ][ static_cast< int >( lang ) ];
}

}   // namespace mrst::updater

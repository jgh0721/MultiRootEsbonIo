#pragma once

#include <QByteArrayView>

namespace mrst::sci {

/// UTF-8 바이트열이 UTF-16 으로 몇 코드 유닛이 되는가.
///
/// 상태바의 "문자 N" 이 쓰는 값이다. 그 숫자는 오래도록
/// `QString::fromUtf8( 문서 전문 ).size()` 였다 — 세는 값 하나를 얻으려고 문서
/// 크기의 memcpy 와 그 2배의 QString 할당을 치렀고, 그 경로는 **캐럿을 움직일
/// 때마다** 돌았다.
///
/// 왜 `SCI_COUNTCODEUNITS` 로 바꾸지 않았는가: 실측이 더 느렸다. 700 KB 문서에서
/// `SCI_COUNTCODEUNITS` 1.89 ms 대 `text()` 복사 0.57 ms 로 **3배 넘게 뒤진다.**
/// 상류 `Document::CountUTF16` 이 문자마다 `NextPosition()` 을 부르는 반면
/// `QString::fromUtf8` 은 SIMD 로 훑기 때문이다. 그래서 세는 방법이 아니라
/// **세는 횟수**를 고쳤다 — 값을 캐시하고 편집분만 더하고 뺀다. 이 함수는 그
/// "편집분" 을 재는 데 쓰이며, 입력이 편집 크기라 문서 크기와 무관하다.
///
/// 셈법: UTF-8 선두 바이트(계속 바이트 `10xxxxxx` 가 아닌 것)마다 코드포인트 하나.
/// 4바이트 시퀀스(`11110xxx`)는 BMP 밖이라 UTF-16 에서 서러게이트 쌍, 즉 2 유닛이다.
///
/// **입력이 올바른 UTF-8 이라고 가정한다.** 이 저장소에서는 성립한다 — 문서는
/// `SC_CP_UTF8` 이고, 적재 경로(`TextFileSession`)가 어떤 인코딩이든 QString 으로
/// 디코딩한 뒤 `toUtf8()` 로 넣는다. 깨진 바이트가 들어오면
/// `QString::fromUtf8` 은 그것을 U+FFFD 하나로 바꿔 세지만 이 함수는 0 으로 세므로
/// 값이 갈린다. 그 경우의 권위는 전체 재계산 쪽이다.
[[nodiscard]] inline int utf16Length( const QByteArrayView utf8 ) noexcept
{
    int units = 0;
    for( const char raw : utf8 )
    {
        const unsigned char byte = static_cast< unsigned char >( raw );
        if( ( byte & 0xC0u ) == 0x80u )
            continue;                       // 계속 바이트 — 앞 코드포인트에 딸린 것
        units += ( ( byte & 0xF8u ) == 0xF0u ) ? 2 : 1;
    }
    return units;
}

}   // namespace mrst::sci

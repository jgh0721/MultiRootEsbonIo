#pragma once

#include <QString>

namespace mrst {

/// Authenticode 서명 확인 결과.
struct SignatureInfo
{
    bool                                trusted = false;   ///< 체인이 신뢰 루트까지 이어지는가
    QString                             subject;           ///< 서명자 주체 (표시용 이름)
    QString                             thumbprintSha1;    ///< 대문자 16진, 구분자 없음
    QString                             errorText;         ///< trusted == false 인 이유
};

/// 파일의 Authenticode 서명을 확인한다.
///
/// **실효성 검사(CRL/OCSP)를 하지 않는다.** 폐쇄망에서 파일당 최대 15초씩
/// 멈추는데, 우리가 알아야 하는 것은 "이 파일이 우리가 서명한 것인가" 하나다.
/// tools/CertWithEV.cmd 가 RFC3161 타임스탬프를 넣으므로 인증서가 만료된
/// 뒤에도 서명 자체는 유효하게 남는다.
[[nodiscard]] SignatureInfo             verifyAuthenticode( const QString& filePath );

/// 우리 배포물인지 판정한다.
///
/// 서명이 유효한 것만으로는 부족하다 — 세상의 모든 유효한 코드 서명 인증서가
/// 통과하기 때문이다. 그래서 우리 인증서로 좁힌다.
[[nodiscard]] bool                      isTrustedPublisher( const SignatureInfo& info );

/// 실행 파일 리소스의 FileVersion 문자열 (예 "0.2.1"). 없으면 빈 문자열.
///
/// 서명과는 다른 관심사지만 둘 다 "내려받은 배포물이 매니페스트가 말하는 그
/// 물건인가" 를 묻는 검사라 같은 파일에 둔다.
[[nodiscard]] QString                   fileVersionString( const QString& filePath );

}  // namespace mrst

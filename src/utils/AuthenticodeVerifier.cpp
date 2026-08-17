#include "stdafx.h"
#include "AuthenticodeVerifier.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
// softpub.h 가 WINTRUST_ACTION_GENERIC_VERIFY_V2 를, wintrust.h 가 WTHelper* 를 준다.
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#endif

namespace mrst {

#ifdef Q_OS_WIN

namespace {

/// 우리 코드 서명 인증서의 SHA-1 지문 (tools/CertWithEV.cmd 의 THUMBPRINT 와 같은 값).
///
/// 지문은 서명된 파일에서 누구나 뽑아낼 수 있는 공개 정보라 소스에 두어도
/// 된다. 그 스크립트가 함께 담고 있는 서명 서버 자격 정보는 절대 옮기지 않는다.
///
/// 인증서를 갱신하면 지문이 바뀐다. 그때 **기존 항목을 지우지 말고 새 지문을
/// 추가**해야 한다. 지우면 구 인증서로 서명된 릴리스에서 새 릴리스로 올라오는
/// 경로가 막힌다(구 클라이언트가 새 파일을 거부하는 것이 아니라, 그 반대로
/// 이미 배포된 클라이언트가 새 서명을 모르는 상황이 문제다 — 그래서 갱신 전에
/// 새 지문을 먼저 추가한 버전을 배포해 두어야 한다).
constexpr const wchar_t* kTrustedThumbprints[] = {
    L"BE4C93738C719A40A27CAD0ABAA11499D7E67348",
};

/// 허용할 서명자 주체명 (CERT_NAME_SIMPLE_DISPLAY_TYPE 이 돌려주는 표기).
///
/// 지문과 주체명 둘 다 두는 이유: 지문은 정확하지만 인증서를 갱신하면 바뀌고,
/// 주체명은 갱신을 견디지만 발급 기관이 표기를 바꾸면 어긋난다. 어느 한쪽이라도
/// 맞으면 통과시켜서, 인증서 갱신이 곧바로 "업데이트 불가" 가 되지 않게 한다.
///
/// 아래 값은 실제로 서명한 배포물에서 읽은 것이다. 표기가 바뀌면 **기존 항목을
/// 지우지 말고 추가**한다 — 이미 배포된 클라이언트는 옛 표기만 알고 있다.
constexpr const wchar_t* kTrustedSubjects[] = {
    L"GYEYOUNG TECHNOLOGY&INFORMATION CO",
};

[[nodiscard]] QString formatLastError( const DWORD code )
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
        reinterpret_cast< wchar_t* >( &buffer ), 0, nullptr );

    QString text;
    if( length > 0 && buffer != nullptr )
        text = QString::fromWCharArray( buffer, static_cast< int >( length ) ).trimmed();
    if( buffer != nullptr )
        LocalFree( buffer );

    if( text.isEmpty() )
        text = QStringLiteral( "0x%1" ).arg( static_cast< quint32 >( code ), 8, 16, QLatin1Char( '0' ) );
    return text;
}

/// WinVerifyTrust 의 상태 코드를 사람이 읽을 수 있는 문장으로.
[[nodiscard]] QString describeTrustStatus( const LONG status )
{
    switch( static_cast< DWORD >( status ) )
    {
        case TRUST_E_NOSIGNATURE:
            return QCoreApplication::translate( "AuthenticodeVerifier", "서명이 없습니다." );
        case TRUST_E_BAD_DIGEST:
            return QCoreApplication::translate( "AuthenticodeVerifier", "서명이 파일 내용과 맞지 않습니다 (파일이 변경되었습니다)." );
        case TRUST_E_EXPLICIT_DISTRUST:
            return QCoreApplication::translate( "AuthenticodeVerifier", "이 서명은 신뢰하지 않도록 지정되어 있습니다." );
        case CERT_E_UNTRUSTEDROOT:
            return QCoreApplication::translate( "AuthenticodeVerifier", "서명 인증서의 루트를 신뢰할 수 없습니다." );
        case CERT_E_CHAINING:
            return QCoreApplication::translate( "AuthenticodeVerifier", "서명 인증서의 체인을 만들 수 없습니다." );
        case CERT_E_EXPIRED:
            return QCoreApplication::translate( "AuthenticodeVerifier", "서명 인증서가 만료되었습니다." );
        default:
            return formatLastError( static_cast< DWORD >( status ) );
    }
}

/// 검증 상태에서 서명자 인증서를 꺼내 주체명과 지문을 채운다.
/// WTD_STATEACTION_CLOSE 전에 불러야 한다.
void fillSignerDetails( WINTRUST_DATA& data, SignatureInfo& info )
{
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData( data.hWVTStateData );
    if( provider == nullptr )
        return;

    CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain( provider, 0, FALSE, 0 );
    if( signer == nullptr )
        return;

    CRYPT_PROVIDER_CERT* providerCert = WTHelperGetProvCertFromChain( signer, 0 );
    if( providerCert == nullptr || providerCert->pCert == nullptr )
        return;

    PCCERT_CONTEXT cert = providerCert->pCert;

    const DWORD nameLength = CertGetNameStringW( cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                                nullptr, nullptr, 0 );
    if( nameLength > 1 )
    {
        QVarLengthArray< wchar_t, 256 > name( static_cast< qsizetype >( nameLength ) );
        if( CertGetNameStringW( cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                               name.data(), nameLength ) > 1 )
        {
            info.subject = QString::fromWCharArray( name.data() );
        }
    }

    BYTE hash[ 20 ] = {};
    DWORD hashSize = sizeof( hash );
    if( CertGetCertificateContextProperty( cert, CERT_SHA1_HASH_PROP_ID, hash, &hashSize ) )
    {
        info.thumbprintSha1 = QString::fromLatin1(
            QByteArray( reinterpret_cast< const char* >( hash ),
                        static_cast< qsizetype >( hashSize ) ).toHex().toUpper() );
    }
}

}  // namespace

SignatureInfo verifyAuthenticode( const QString& filePath )
{
    SignatureInfo info;

    const QFileInfo fileInfo( filePath );
    if( !fileInfo.isFile() )
    {
        info.errorText = QCoreApplication::translate( "AuthenticodeVerifier", "파일이 없습니다: %1" ).arg( filePath );
        return info;
    }

    const QString nativePath = QDir::toNativeSeparators( fileInfo.absoluteFilePath() );
    std::wstring widePath( static_cast< size_t >( nativePath.size() ) + 1, L'\0' );
    widePath.resize( static_cast< size_t >( nativePath.toWCharArray( widePath.data() ) ) );

    WINTRUST_FILE_INFO fileData{};
    fileData.cbStruct      = sizeof( fileData );
    fileData.pcwszFilePath = widePath.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof( data );
    // UI 를 절대 띄우지 않는다. 이 검사는 백그라운드에서 돈다.
    data.dwUIChoice = WTD_UI_NONE;
    // 실효성 검사를 건너뛰는 이유는 헤더 주석에 적어 두었다.
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice       = WTD_CHOICE_FILE;
    data.pFile               = &fileData;
    // hWVTStateData 를 받아 서명자 정보를 꺼내려면 VERIFY/CLOSE 로 나눠야 한다.
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    // 네트워크로 나가지 않는다 (WTD_CACHE_ONLY_URL_RETRIEVAL).
    data.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action        = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status  = WinVerifyTrust( nullptr, &action, &data );

    // 서명자 정보는 상태를 닫기 전에 꺼낸다. 서명이 신뢰되지 않는 경우에도
    // 주체/지문은 로그에 남겨 두는 편이 원인 파악에 도움이 된다.
    fillSignerDetails( data, info );

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust( nullptr, &action, &data );

    info.trusted = ( status == ERROR_SUCCESS );
    if( !info.trusted )
        info.errorText = describeTrustStatus( status );

    return info;
}

bool isTrustedPublisher( const SignatureInfo& info )
{
    if( !info.trusted )
        return false;

    for( const wchar_t* thumbprint : kTrustedThumbprints )
    {
        if( thumbprint == nullptr )
            continue;
        if( info.thumbprintSha1.compare( QString::fromWCharArray( thumbprint ),
                                        Qt::CaseInsensitive ) == 0 )
        {
            return true;
        }
    }

    for( const wchar_t* subject : kTrustedSubjects )
    {
        if( subject == nullptr )
            continue;
        if( info.subject.compare( QString::fromWCharArray( subject ), Qt::CaseInsensitive ) == 0 )
            return true;
    }

    return false;
}

QString fileVersionString( const QString& filePath )
{
    const QFileInfo fileInfo( filePath );
    if( !fileInfo.isFile() )
        return {};

    const QString nativePath = QDir::toNativeSeparators( fileInfo.absoluteFilePath() );
    std::wstring widePath( static_cast< size_t >( nativePath.size() ) + 1, L'\0' );
    widePath.resize( static_cast< size_t >( nativePath.toWCharArray( widePath.data() ) ) );

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW( widePath.c_str(), &handle );
    if( size == 0 )
        return {};

    QByteArray buffer( static_cast< qsizetype >( size ), Qt::Uninitialized );
    if( !GetFileVersionInfoW( widePath.c_str(), handle, size, buffer.data() ) )
        return {};

    // 언어/코드페이지 블록을 먼저 물어본다. 우리 app.rc 는 040904B0 고정이지만,
    // 하드코딩하면 리소스 설정이 바뀌는 순간 조용히 빈 값을 돌려주게 된다.
    struct LangCodePage
    {
        WORD language;
        WORD codePage;
    };

    LangCodePage* translations = nullptr;
    UINT translationBytes      = 0;
    QList< LangCodePage > candidates;
    if( VerQueryValueW( buffer.constData(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast< void** >( &translations ), &translationBytes )
        && translations != nullptr )
    {
        const UINT count = translationBytes / sizeof( LangCodePage );
        for( UINT index = 0; index < count; ++index )
            candidates.append( translations[ index ] );
    }
    // 블록이 없으면 영어(미국)/Unicode 를 시도한다.
    candidates.append( LangCodePage{ 0x0409, 1200 } );

    for( const LangCodePage& candidate : candidates )
    {
        const QString subBlock = QStringLiteral( "\\StringFileInfo\\%1%2\\FileVersion" )
                                    .arg( candidate.language, 4, 16, QLatin1Char( '0' ) )
                                    .arg( candidate.codePage, 4, 16, QLatin1Char( '0' ) );
        std::wstring wideBlock( static_cast< size_t >( subBlock.size() ) + 1, L'\0' );
        wideBlock.resize( static_cast< size_t >( subBlock.toWCharArray( wideBlock.data() ) ) );

        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if( VerQueryValueW( buffer.constData(), wideBlock.c_str(),
                           reinterpret_cast< void** >( &value ), &valueLength )
            && value != nullptr && valueLength > 0 )
        {
            return QString::fromWCharArray( value, static_cast< int >( valueLength ) ).trimmed();
        }
    }

    return {};
}

#else

SignatureInfo verifyAuthenticode( const QString& )
{
    SignatureInfo info;
    info.errorText = QCoreApplication::translate( "AuthenticodeVerifier", "이 플랫폼에서는 서명을 확인할 수 없습니다." );
    return info;
}

bool isTrustedPublisher( const SignatureInfo& )
{
    return false;
}

QString fileVersionString( const QString& )
{
    return {};
}

#endif

}  // namespace mrst

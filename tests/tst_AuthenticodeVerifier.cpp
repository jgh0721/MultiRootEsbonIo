#include "TestRunner.hpp"

#include "utils/AuthenticodeVerifier.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

using namespace mrst;

namespace {

/// 반드시 존재하고 Microsoft 가 서명한 파일. 검증기가 "정상 서명" 을 실제로
/// 알아보는지 확인하는 기준점이다.
QString systemSignedFile()
{
    const QString root = QProcessEnvironment::systemEnvironment().value( QStringLiteral( "SystemRoot" ),
                                                                        QStringLiteral( "C:/Windows" ) );
    return QDir( root ).filePath( QStringLiteral( "System32/kernel32.dll" ) );
}

}  // namespace

/// 이 검증기는 자동 업데이트의 신뢰 경계다. sha256 은 매니페스트와 같은 채널로
/// 오기 때문에 프록시가 자체 루트로 가로채면 둘 다 위조할 수 있고, 그때 남는
/// 방어선은 서명 확인 하나뿐이다. 느슨하면 임의의 실행 파일이 설치된다.
class TestAuthenticodeVerifier : public QObject
{
    Q_OBJECT

private slots:
    void reportsMissingFile();
    void recognisesSystemSignature();
    void rejectsUnsignedFile();
    void rejectsTamperedFile();
    void doesNotTrustForeignPublisher();
    void readsFileVersion();
    void acceptsOurOwnSignedBuild();
};

void TestAuthenticodeVerifier::reportsMissingFile()
{
    const SignatureInfo info = verifyAuthenticode( QStringLiteral( "Z:/없는파일.exe" ) );
    QVERIFY( !info.trusted );
    QVERIFY( !info.errorText.isEmpty() );
    QVERIFY( !isTrustedPublisher( info ) );
}

void TestAuthenticodeVerifier::recognisesSystemSignature()
{
    const QString path = systemSignedFile();
    if( !QFileInfo::exists( path ) )
        QSKIP( "kernel32.dll 을 찾을 수 없다" );

    const SignatureInfo info = verifyAuthenticode( path );
    QVERIFY2( info.trusted, qPrintable( info.errorText ) );
    QVERIFY( !info.subject.isEmpty() );
    // 지문은 SHA-1 이라 항상 40자 16진이다.
    QCOMPARE( info.thumbprintSha1.size(), 40 );
}

void TestAuthenticodeVerifier::rejectsUnsignedFile()
{
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );

    const QString path = dir.filePath( QStringLiteral( "unsigned.exe" ) );
    QFile file( path );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( QByteArrayLiteral( "MZ not really an executable" ) );
    file.close();

    const SignatureInfo info = verifyAuthenticode( path );
    QVERIFY( !info.trusted );
    QVERIFY( !isTrustedPublisher( info ) );
}

void TestAuthenticodeVerifier::rejectsTamperedFile()
{
    const QString source = systemSignedFile();
    if( !QFileInfo::exists( source ) )
        QSKIP( "kernel32.dll 을 찾을 수 없다" );

    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString copy = dir.filePath( QStringLiteral( "tampered.dll" ) );
    QVERIFY( QFile::copy( source, copy ) );

    // 서명된 파일의 본문을 한 바이트 고치면 다이제스트가 어긋나야 한다.
    // 이것이 깨지면 "내려받는 중 손상" 과 "누가 바꿔치기" 를 구분하지 못한다.
    {
        QFile file( copy );
        QVERIFY( file.open( QIODevice::ReadWrite ) );
        QVERIFY( file.seek( file.size() / 2 ) );
        char byte = 0;
        QVERIFY( file.read( &byte, 1 ) == 1 );
        QVERIFY( file.seek( file.size() / 2 ) );
        byte = static_cast< char >( byte ^ 0xFF );
        QVERIFY( file.write( &byte, 1 ) == 1 );
    }

    const SignatureInfo info = verifyAuthenticode( copy );
    QVERIFY( !info.trusted );
    QVERIFY( !isTrustedPublisher( info ) );
}

void TestAuthenticodeVerifier::doesNotTrustForeignPublisher()
{
    const QString path = systemSignedFile();
    if( !QFileInfo::exists( path ) )
        QSKIP( "kernel32.dll 을 찾을 수 없다" );

    const SignatureInfo info = verifyAuthenticode( path );
    QVERIFY( info.trusted );
    // 서명이 유효한 것만으로는 부족하다. 세상의 모든 유효한 코드 서명 인증서가
    // 통과하면 업데이트 검증은 아무 것도 막지 못한다.
    QVERIFY( !isTrustedPublisher( info ) );
}

void TestAuthenticodeVerifier::readsFileVersion()
{
    const QString path = systemSignedFile();
    if( !QFileInfo::exists( path ) )
        QSKIP( "kernel32.dll 을 찾을 수 없다" );

    const QString version = fileVersionString( path );
    QVERIFY( !version.isEmpty() );
    QVERIFY( version.at( 0 ).isDigit() );

    // 버전 리소스가 없는 파일은 빈 문자열이어야 한다 (예외가 아니라).
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString plain = dir.filePath( QStringLiteral( "plain.txt" ) );
    QFile file( plain );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( QByteArrayLiteral( "no version resource here" ) );
    file.close();
    QVERIFY( fileVersionString( plain ).isEmpty() );
}

void TestAuthenticodeVerifier::acceptsOurOwnSignedBuild()
{
    // 서명된 배포물이 있을 때만 도는 검사다. mrst_package 로 패키지를 만든 뒤
    // 그 경로를 넘기면 "우리 인증서를 실제로 알아보는가" 까지 확인할 수 있다:
    //   set MRST_TEST_SIGNED_EXE=<package>\MultiRoot-reST-Editor-0.4.0\MultiRoot-reST Editor.exe
    const QString path = QProcessEnvironment::systemEnvironment()
                            .value( QStringLiteral( "MRST_TEST_SIGNED_EXE" ) );
    if( path.isEmpty() || !QFileInfo::exists( path ) )
        QSKIP( "MRST_TEST_SIGNED_EXE 가 지정되지 않았다" );

    const SignatureInfo info = verifyAuthenticode( path );
    QVERIFY2( info.trusted, qPrintable( info.errorText ) );
    QVERIFY2( isTrustedPublisher( info ),
             qPrintable( QStringLiteral( "우리 배포물로 인정하지 못했다. subject='%1'" )
                            .arg( info.subject ) ) );
}

MRST_REGISTER_TEST( TestAuthenticodeVerifier );

#include "tst_AuthenticodeVerifier.moc"

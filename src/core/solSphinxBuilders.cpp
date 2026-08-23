#include "stdafx.h"
#include "core/solSphinxBuilders.hpp"

#include <QDir>
#include <QRegularExpression>

namespace mrst {

QStringList sphinxBuilderPresets()
{
    return { QStringLiteral( "html" ),     QStringLiteral( "dirhtml" ),
            QStringLiteral( "singlehtml" ), QStringLiteral( "latexpdf" ),
            QStringLiteral( "epub" ),      QStringLiteral( "text" ),
            QStringLiteral( "man" ) };
}

bool isValidSphinxBuilderName( const QString& builder )
{
    // 첫 글자는 영숫자여야 한다. 하이픈으로 시작하는 것을 허용하면 `-D` 같은
    // 값이 그대로 통과해 sphinx-build 가 그것을 **옵션**으로 읽는다.
    static const QRegularExpression nameRe( QStringLiteral( R"(^[A-Za-z0-9][A-Za-z0-9_-]*$)" ) );
    return nameRe.match( builder.trimmed() ).hasMatch();
}

QString sphinxMakeModeSubdirectory( const QString& builder )
{
    const QString name = builder.trimmed().toLower();
    if( name == QLatin1String( "latexpdf" ) || name == QLatin1String( "latexpdfja" ) )
        return QStringLiteral( "latex" );
    if( name == QLatin1String( "info" ) )
        return QStringLiteral( "texinfo" );
    return {};
}

bool isSphinxMakeModeTarget( const QString& builder )
{
    return !sphinxMakeModeSubdirectory( builder ).isEmpty();
}

QString defaultSphinxOutputDirectory( const QString& projectRoot, const QString& builder )
{
    if( projectRoot.isEmpty() )
        return {};

    const QString name = builder.trimmed().isEmpty() ? QStringLiteral( "html" ) : builder.trimmed();
    return QDir::toNativeSeparators(
        QDir( projectRoot ).absoluteFilePath( QStringLiteral( "_build/%1" ).arg( name ) ) );
}

}  // namespace mrst

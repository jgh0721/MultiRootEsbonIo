#pragma once

#include <QObject>
#include <QDateTime>
#include <QString>

class QWidget;

namespace mrst {

class PythonEnvManager final : public QObject
{
    Q_OBJECT

public:
    explicit PythonEnvManager( QObject* parent = nullptr );

    [[nodiscard]] QString               runtimeRoot() const;
    [[nodiscard]] QString               projectDir() const;
    [[nodiscard]] QString               venvDir() const;
    [[nodiscard]] QString               pythonExe() const;
    [[nodiscard]] QString               sphinxBuildExe() const;
    [[nodiscard]] QString               esbonioExe() const;
    [[nodiscard]] QString               embeddedUvTarget() const;
    [[nodiscard]] QString               readyMarker() const;

    [[nodiscard]] bool                  useExternalUv() const;
    [[nodiscard]] QString               externalUvPath() const;
    void                                setUseExternalUv( bool enabled );
    void                                setExternalUvPath( const QString& path );
    void                                saveUvSettings() const;

    [[nodiscard]] bool                  isReady() const;
    [[nodiscard]] QDateTime             configuredDate() const;
    [[nodiscard]] QString               configuredDateText() const;
    [[nodiscard]] QString               uvDescription() const;
    [[nodiscard]] QString               lastError() const;

    bool                                ensureEnvironment( QWidget* dialogParent = nullptr );
    bool                                configureEnvironment( QWidget* dialogParent = nullptr );

signals:
    void                                bootstrapLog( const QString& text );

private:
    [[nodiscard]] QString               appDir() const;
    [[nodiscard]] QString               resourcePyprojectSource() const;
    [[nodiscard]] QString               resourceUvSource() const;
    [[nodiscard]] QString               uvExecutable() const;
    [[nodiscard]] QString               uvVersionText( const QString& uvPath ) const;

    bool                                prepareProjectFiles( QString* errorMessage );
    bool                                runUvSync( QString* errorMessage );
    bool                                copyResourceFile( const QString& resourcePath, const QString& destinationPath,
                                                          bool executable, QString* errorMessage ) const;
    void                                setLastError( const QString& errorMessage );

    bool                                m_useExternalUv = false;
    QString                             m_externalUvPath;
    QString                             m_lastError;
};

}  // namespace mrst



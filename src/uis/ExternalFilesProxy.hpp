#pragma once

#include <QIdentityProxyModel>
#include <QPersistentModelIndex>
#include <QStringList>

namespace mrst {

/// Preserves the lazy filesystem tree and appends a virtual External branch
/// under the displayed workspace root. External files are references, not copies.
class ExternalFilesProxy final : public QIdentityProxyModel
{
    Q_OBJECT
public:
    explicit ExternalFilesProxy( QObject* parent = nullptr );
    void setSourceModel( QAbstractItemModel* source ) override;
    void setWorkspace( const QString& path, const QModelIndex& root );
    bool addFile( const QString& path );
    void removeFile( const QString& path );
    QStringList files() const { return files_; }
    QModelIndex externalRoot() const;
    QModelIndex indexForFile( const QString& path ) const;
    QString filePath( const QModelIndex& index ) const;
    bool isExternalRoot( const QModelIndex& index ) const;

    QModelIndex mapToSource( const QModelIndex& index ) const override;
    QItemSelection mapSelectionToSource( const QItemSelection& selection ) const override;
    QModelIndex buddy( const QModelIndex& index ) const override;
    QMap<int, QVariant> itemData( const QModelIndex& index ) const override;
    QModelIndex index( int row, int column, const QModelIndex& parent = {} ) const override;
    QModelIndex parent( const QModelIndex& index ) const override;
    QModelIndex sibling( int row, int column, const QModelIndex& index ) const override;
    int rowCount( const QModelIndex& parent = {} ) const override;
    int columnCount( const QModelIndex& parent = {} ) const override;
    bool hasChildren( const QModelIndex& parent = {} ) const override;
    bool canFetchMore( const QModelIndex& parent ) const override;
    void fetchMore( const QModelIndex& parent ) override;
    QVariant data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    bool setData( const QModelIndex& index, const QVariant& value, int role ) override;
    Qt::ItemFlags flags( const QModelIndex& index ) const override;

private:
    bool isExternalFile( const QModelIndex& index ) const;
    bool atRoot( const QModelIndex& parent ) const;
    QString workspace_;
    QPersistentModelIndex root_;
    QStringList files_;
    char rootTag_ = 0;
    char fileTag_ = 0;
    QList<QMetaObject::Connection> connections_;
    QModelIndexList layoutIndexes_;
    QList<QPersistentModelIndex> layoutSources_;
};

} // namespace mrst

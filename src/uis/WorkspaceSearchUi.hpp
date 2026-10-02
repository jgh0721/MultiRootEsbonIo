#pragma once

#include <DockWidget.h>
#include <QApplication>
#include <QLineEdit>
#include <QTimer>

namespace mrst {

inline void showWorkspaceSearchDock( ads::CDockWidget* dock, QLineEdit* query )
{
    dock->toggleView( true );
    dock->raise();
    dock->setAsCurrentTab();
    query->setFocus( Qt::ShortcutFocusReason );
    query->selectAll();
    // Floating-window activation can restore the old focus after raise().
    // Finish focus/selection after the dock and native window have activated.
    QTimer::singleShot( 0, query, [query] {
        if( !query->isVisible() ) return;
        query->setFocus( Qt::ShortcutFocusReason );
        query->selectAll();
    } );
}

} // namespace mrst

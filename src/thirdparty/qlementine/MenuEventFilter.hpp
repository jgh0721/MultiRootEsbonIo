// SPDX-FileCopyrightText: Olivier Cléro <oclero@hotmail.com>
// SPDX-License-Identifier: MIT

// Local patch: leave mouse activation to QMenu. Upstream consumes the release,
// flashes the action, then synthesizes another release. When stylesheet polish
// installs multiple filters, those releases are consumed again and the menu
// stays open, retaining the popup mouse grab. Preserve only shadow positioning.
// The deferred geometry callback belongs to this filter, not its parent menu.

#pragma once

#include <oclero/qlementine/style/QlementineStyle.hpp>

#include <QEvent>
#include <QObject>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>

namespace oclero::qlementine {
class MenuEventFilter : public QObject {
public:
  explicit MenuEventFilter(QMenu* menu)
    : QObject(menu)
    , _menu(menu) {
    menu->installEventFilter(this);
  }

  bool eventFilter(QObject*, QEvent* evt) override {
    switch (evt->type()) {
      case QEvent::Type::Show: {
        // Place the QMenu correctly by making up for the drop shadow margins.
        // It'll be reset before every show, so we can safely move it every time.
        // Submenus should already be placed correctly, so there's no need to translate their geometry.
        // Also, make up for the menu item padding so the texts are aligned.
        const auto isMenuBarMenu = qobject_cast<QMenuBar*>(_menu->parentWidget()) != nullptr;
        const auto isSubMenu = qobject_cast<QMenu*>(_menu->parentWidget()) != nullptr;
        const auto alignForMenuBar = isMenuBarMenu && !isSubMenu;
        const auto* qlementineStyle = qobject_cast<QlementineStyle*>(_menu->style());
        const auto menuItemHPadding = qlementineStyle ? qlementineStyle->theme().spacing : 0;
        const auto menuDropShadowWidth = qlementineStyle ? qlementineStyle->theme().spacing : 0;
        const auto menuOriginalPos = _menu->pos();
        const auto menuBarTranslation = alignForMenuBar ? QPoint(-menuItemHPadding, 0) : QPoint(0, 0);
        const auto shadowTranslation = QPoint(-menuDropShadowWidth, -menuDropShadowWidth);
        const auto menuNewPos = menuOriginalPos + menuBarTranslation + shadowTranslation;

        // Menus have weird sizing bugs when moving them from this event.
        // We have to wait for the event loop to be processed before setting the final position.
        const auto menuSize = _menu->size();
        if (menuSize != QSize(0, 0)) {
          _menu->resize(0, 0); // Hide the menu for now until we can set the position.
          QTimer::singleShot(0, this, [this, menuNewPos, menuSize]() {
            _menu->move(menuNewPos);
            _menu->resize(menuSize);
          });
        }
      } break;
      default:
        break;
    }

    return false;
  }

private:
  QMenu* _menu{ nullptr };

};
} // namespace oclero::qlementine

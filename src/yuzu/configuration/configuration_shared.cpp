// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <type_traits>
#include <vector>
#include <QLayout>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>
#include "yuzu/configuration/configuration_shared.h"

namespace ConfigurationShared {

Tab::Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent) : QWidget(parent) {
    if (group != nullptr) {
        group->push_back(this);
    }
}

Tab::~Tab() = default;

namespace {

void MakePageScrollable(QWidget* page) {
    QLayout* page_layout = page->layout();
    if (page_layout == nullptr || qobject_cast<QScrollArea*>(page) != nullptr) {
        return;
    }
    if (page->findChild<QScrollArea*>() != nullptr) {
        // Wrap the sub-pages instead, so no scroll area ends up inside another one.
        for (QTabWidget* tabs : page->findChildren<QTabWidget*>(Qt::FindDirectChildrenOnly)) {
            for (int index = 0; index < tabs->count(); ++index) {
                MakePageScrollable(tabs->widget(index));
            }
        }
        return;
    }
    page_layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* content = new QWidget;
    content->setLayout(page_layout);

    auto* scroll = new QScrollArea(page);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

} // Anonymous namespace

void FitToScreen(QWidget* window, const QList<QWidget*>& pages) {
    // QDialog places itself on its parent's screen once shown, so measure that one.
    const QWidget* parent = window->parentWidget();
    const QScreen* screen = (parent != nullptr ? parent->window() : window)->screen();
    if (screen == nullptr) {
        return;
    }
    window->ensurePolished();
    QSize available = screen->availableGeometry().size();
    available.rheight() -= window->style()->pixelMetric(QStyle::PM_TitleBarHeight);
    const QSize needed = window->size().expandedTo(window->minimumSizeHint());
    if (needed.width() <= available.width() && needed.height() <= available.height()) {
        return;
    }
    for (QWidget* page : pages) {
        MakePageScrollable(page);
    }
    window->setMinimumSize(0, 0);
    window->resize(needed.boundedTo(available));
}

} // namespace ConfigurationShared

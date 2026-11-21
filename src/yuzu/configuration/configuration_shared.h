// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QList>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>

class QObject;

namespace ConfigurationShared {

class Tab : public QWidget {
    Q_OBJECT

public:
    explicit Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent = nullptr);
    ~Tab();

    virtual void ApplyConfiguration() = 0;
    virtual void SetConfiguration() = 0;
};

/// If the window does not fit its screen, puts the pages into scroll areas and shrinks the window.
void FitToScreen(QWidget* window, const QList<QWidget*>& pages);

} // namespace ConfigurationShared

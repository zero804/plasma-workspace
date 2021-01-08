/*
 *  This file is part of the KDE libraries
 *  Copyright (c) 2001 Waldo Bastian <bastian@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License version 2 as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 **/

#include "autostart.h"

#include <QDir>
#include <QHash>
#include <QStandardPaths>
#include <kautostart.h>

AutoStart::AutoStart()
    : m_phase(-1)
    , m_phasedone(false)
{
    loadAutoStartList();
}

AutoStart::~AutoStart()
{
}

void AutoStart::setPhase(int phase)
{
    if (phase > m_phase) {
        m_phase = phase;
        m_phasedone = false;
    }
}

void AutoStart::setPhaseDone()
{
    m_phasedone = true;
}

static QString extractName(QString path) // krazy:exclude=passbyvalue
{
    int i = path.lastIndexOf(QLatin1Char('/'));
    if (i >= 0) {
        path = path.mid(i + 1);
    }
    i = path.lastIndexOf(QLatin1Char('.'));
    if (i >= 0) {
        path.truncate(i);
    }
    return path;
}

void AutoStart::loadAutoStartList()
{
    // XDG autostart dirs

    // Make unique list of relative paths
    QHash<QString, QString> files;
    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, QStringLiteral("autostart"), QStandardPaths::LocateDirectory);
    for (const QString &dir : dirs) {
        const QDir d(dir);
        const QStringList fileNames = d.entryList(QStringList() << QStringLiteral("*.desktop"));
        for (const QString &file : fileNames) {
            if (!files.contains(file)) {
                files.insert(file, d.absoluteFilePath(file));
            }
        }
    }

    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        KAutostart config(*it);
        if (!config.autostarts(QStringLiteral("KDE"), KAutostart::CheckAll)) {
            continue;
        }

        AutoStartItem item;
        item.service = *it;
        item.name = extractName(it.key());
        item.startAfter = config.startAfter();
        item.phase = qMax(KAutostart::BaseDesktop, config.startPhase());
        m_startList.append(item);
    }
}

QString AutoStart::startService()
{
    if (m_startList.isEmpty()) {
        return QString();
    }

    while (!m_started.isEmpty()) {
        // Check for items that depend on previously started items
        QString lastItem = m_started[0];
        QMutableVectorIterator<AutoStartItem> it(m_startList);
        while (it.hasNext()) {
            const auto &item = it.next();
            if (item.phase == m_phase && item.startAfter == lastItem) {
                m_started.prepend(item.name);
                QString service = item.service;
                it.remove();
                return service;
            }
        }
        m_started.removeFirst();
    }

    // Check for items that don't depend on anything
    QMutableVectorIterator<AutoStartItem> it(m_startList);
    while (it.hasNext()) {
        const auto &item = it.next();
        if (item.phase == m_phase && item.startAfter.isEmpty()) {
            m_started.prepend(item.name);
            QString service = item.service;
            it.remove();
            return service;
        }
    }

    // Just start something in this phase
    it = m_startList;
    while (it.hasNext()) {
        const auto &item = it.next();
        if (item.phase == m_phase) {
            m_started.prepend(item.name);
            QString service = item.service;
            it.remove();
            return service;
        }
    }

    return QString();
}

QVector<AutoStartItem> AutoStart::startList() const
{
    QVector<AutoStartItem> ret;
    for (const auto &asi : m_startList) {
        if (asi.phase == m_phase)
            ret << asi;
    }
    return ret;
}

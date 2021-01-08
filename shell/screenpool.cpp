/*
 *  Copyright 2016 Marco Martin <mart@kde.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "screenpool.h"
#include <config-plasma.h>

#include <KWindowSystem>
#include <QGuiApplication>
#include <QScreen>

#if HAVE_X11
#include <QX11Info>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#endif

ScreenPool::ScreenPool(const KSharedConfig::Ptr &config, QObject *parent)
    : QObject(parent)
    , m_configGroup(KConfigGroup(config, QStringLiteral("ScreenConnectors")))
{
    m_configSaveTimer.setSingleShot(true);
    connect(&m_configSaveTimer, &QTimer::timeout, this, [this]() {
        m_configGroup.sync();
    });

#if HAVE_X11
    if (KWindowSystem::isPlatformX11()) {
        qApp->installNativeEventFilter(this);
        const xcb_query_extension_reply_t *reply = xcb_get_extension_data(QX11Info::connection(), &xcb_randr_id);
        m_xrandrExtensionOffset = reply->first_event;
    }
#endif
}

void ScreenPool::load()
{
    m_primaryConnector = QString();
    m_connectorForId.clear();
    m_idForConnector.clear();

    QScreen *primary = qGuiApp->primaryScreen();
    if (primary) {
        m_primaryConnector = primary->name();
        if (!m_primaryConnector.isEmpty()) {
            m_connectorForId[0] = m_primaryConnector;
            m_idForConnector[m_primaryConnector] = 0;
        }
    }

    // restore the known ids to connector mappings
    const auto keys = m_configGroup.keyList();
    for (const QString &key : keys) {
        QString connector = m_configGroup.readEntry(key, QString());
        if (!key.isEmpty() && !connector.isEmpty() && !m_connectorForId.contains(key.toInt()) && !m_idForConnector.contains(connector)) {
            m_connectorForId[key.toInt()] = connector;
            m_idForConnector[connector] = key.toInt();
        } else if (m_idForConnector.value(connector) != key.toInt()) {
            m_configGroup.deleteEntry(key);
        }
    }

    // if there are already connected unknown screens, map those
    // all needs to be populated as soon as possible, otherwise
    // containment->screen() will return an incorrect -1
    // at startup, if it' asked before corona::addOutput()
    // is performed, driving to the creation of a new containment
    for (QScreen *screen : qGuiApp->screens()) {
        if (!m_idForConnector.contains(screen->name())) {
            insertScreenMapping(firstAvailableId(), screen->name());
        }
    }
}

ScreenPool::~ScreenPool()
{
    m_configGroup.sync();
}

QString ScreenPool::primaryConnector() const
{
    return m_primaryConnector;
}

void ScreenPool::setPrimaryConnector(const QString &primary)
{
    if (m_primaryConnector == primary) {
        return;
    }

    int oldIdForPrimary = -1;
    if (m_idForConnector.contains(primary)) {
        oldIdForPrimary = m_idForConnector.value(primary);
    }

    if (oldIdForPrimary == -1) {
        // move old primary to new free id
        oldIdForPrimary = firstAvailableId();
        insertScreenMapping(oldIdForPrimary, m_primaryConnector);
    }

    m_idForConnector[primary] = 0;
    m_connectorForId[0] = primary;
    m_idForConnector[m_primaryConnector] = oldIdForPrimary;
    m_connectorForId[oldIdForPrimary] = m_primaryConnector;
    m_primaryConnector = primary;
    save();
}

void ScreenPool::save()
{
    QMap<int, QString>::const_iterator i;
    for (i = m_connectorForId.constBegin(); i != m_connectorForId.constEnd(); ++i) {
        m_configGroup.writeEntry(QString::number(i.key()), i.value());
    }
    // write to disck every 30 seconds at most
    m_configSaveTimer.start(30000);
}

void ScreenPool::insertScreenMapping(int id, const QString &connector)
{
    Q_ASSERT(!m_connectorForId.contains(id) || m_connectorForId.value(id) == connector);
    Q_ASSERT(!m_idForConnector.contains(connector) || m_idForConnector.value(connector) == id);

    if (id == 0) {
        m_primaryConnector = connector;
    }

    m_connectorForId[id] = connector;
    m_idForConnector[connector] = id;
    save();
}

int ScreenPool::id(const QString &connector) const
{
    if (!m_idForConnector.contains(connector)) {
        return -1;
    }

    return m_idForConnector.value(connector);
}

QString ScreenPool::connector(int id) const
{
    Q_ASSERT(m_connectorForId.contains(id));

    return m_connectorForId.value(id);
}

int ScreenPool::firstAvailableId() const
{
    int i = 0;
    // find the first integer not stored in m_connectorForId
    // m_connectorForId is the only map, so the ids are sorted
    foreach (int existingId, m_connectorForId.keys()) {
        if (i != existingId) {
            return i;
        }
        ++i;
    }

    return i;
}

QList<int> ScreenPool::knownIds() const
{
    return m_connectorForId.keys();
}

bool ScreenPool::nativeEventFilter(const QByteArray &eventType, void *message, long int *result)
{
    Q_UNUSED(result);
#if HAVE_X11
    // a particular edge case: when we switch the only enabled screen
    // we don't have any signal about it, the primary screen changes but we have the same old QScreen* getting recycled
    // see https://bugs.kde.org/show_bug.cgi?id=373880
    // if this slot will be invoked many times, their//second time on will do nothing as name and primaryconnector will be the same by then
    if (eventType[0] != 'x') {
        return false;
    }

    xcb_generic_event_t *ev = static_cast<xcb_generic_event_t *>(message);

    const auto responseType = XCB_EVENT_RESPONSE_TYPE(ev);

    if (responseType == m_xrandrExtensionOffset + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        if (qGuiApp->primaryScreen()->name() != primaryConnector()) {
            // new screen?
            if (id(qGuiApp->primaryScreen()->name()) < 0) {
                insertScreenMapping(firstAvailableId(), qGuiApp->primaryScreen()->name());
            }
            // switch the primary screen in the pool
            setPrimaryConnector(qGuiApp->primaryScreen()->name());
        }
    }
#endif
    return false;
}

#include "moc_screenpool.cpp"

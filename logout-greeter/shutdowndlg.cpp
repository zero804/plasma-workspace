/*****************************************************************
Copyright 2000 Matthias Ettrich <ettrich@kde.org>
Copyright 2007 Urs Wolfer <uwolfer @ kde.org>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

******************************************************************/

#include "shutdowndlg.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QFile>
#include <QPainter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickView>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QX11Info>

#include <KPackage/Package>
#include <KPackage/PackageLoader>

#include <KAuthorized>
#include <KConfigGroup>
#include <KDeclarative/KDeclarative>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KUser>
#include <KWindowEffects>
#include <KWindowSystem>

#include <netwm.h>
#include <stdio.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <fixx11h.h>

#include <config-workspace.h>
#include <debug.h>

#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>

static const QString s_login1Service = QStringLiteral("org.freedesktop.login1");
static const QString s_login1Path = QStringLiteral("/org/freedesktop/login1");
static const QString s_dbusPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
static const QString s_login1ManagerInterface = QStringLiteral("org.freedesktop.login1.Manager");
static const QString s_login1RebootToFirmwareSetup = QStringLiteral("RebootToFirmwareSetup");

KSMShutdownDlg::KSMShutdownDlg(QWindow *parent, KWorkSpace::ShutdownType sdtype, KWayland::Client::PlasmaShell *plasmaShell)
    : QuickViewSharedEngine(parent)
    , m_result(false)
    , m_waylandPlasmaShell(plasmaShell)
// this is a WType_Popup on purpose. Do not change that! Not
// having a popup here has severe side effects.
{
    // window stuff
    setClearBeforeRendering(true);
    setColor(QColor(Qt::transparent));

    setResizeMode(KQuickAddons::QuickViewSharedEngine::SizeRootObjectToView);

    // Qt doesn't set this on unmanaged windows
    // FIXME: or does it?
    if (KWindowSystem::isPlatformX11()) {
        XChangeProperty(QX11Info::display(),
                        winId(),
                        XInternAtom(QX11Info::display(), "WM_WINDOW_ROLE", False),
                        XA_STRING,
                        8,
                        PropModeReplace,
                        (unsigned char *)"logoutdialog",
                        strlen("logoutdialog"));

        XClassHint classHint;
        classHint.res_name = const_cast<char *>("ksmserver");
        classHint.res_class = const_cast<char *>("ksmserver");
        XSetClassHint(QX11Info::display(), winId(), &classHint);
    }

    // QQuickView *windowContainer = QQuickView::createWindowContainer(m_view, this);
    // windowContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    QQmlContext *context = rootContext();
    context->setContextProperty(QStringLiteral("maysd"), m_session.canShutdown());
    context->setContextProperty(QStringLiteral("sdtype"), sdtype);

    QQmlPropertyMap *mapShutdownType = new QQmlPropertyMap(this);
    mapShutdownType->insert(QStringLiteral("ShutdownTypeDefault"), QVariant::fromValue<int>(KWorkSpace::ShutdownTypeDefault));
    mapShutdownType->insert(QStringLiteral("ShutdownTypeNone"), QVariant::fromValue<int>(KWorkSpace::ShutdownTypeNone));
    mapShutdownType->insert(QStringLiteral("ShutdownTypeReboot"), QVariant::fromValue<int>(KWorkSpace::ShutdownTypeReboot));
    mapShutdownType->insert(QStringLiteral("ShutdownTypeHalt"), QVariant::fromValue<int>(KWorkSpace::ShutdownTypeHalt));
    mapShutdownType->insert(QStringLiteral("ShutdownTypeLogout"), QVariant::fromValue<int>(KWorkSpace::ShutdownTypeLogout));
    context->setContextProperty(QStringLiteral("ShutdownType"), mapShutdownType);

    QQmlPropertyMap *mapSpdMethods = new QQmlPropertyMap(this);
    mapSpdMethods->insert(QStringLiteral("StandbyState"), m_session.canSuspend());
    mapSpdMethods->insert(QStringLiteral("SuspendState"), m_session.canSuspend());
    mapSpdMethods->insert(QStringLiteral("HibernateState"), m_session.canHibernate());
    context->setContextProperty(QStringLiteral("spdMethods"), mapSpdMethods);
    context->setContextProperty(QStringLiteral("canLogout"), m_session.canLogout());

    // Trying to access a non-existent context property throws an error, always create the property and then update it later
    context->setContextProperty("rebootToFirmwareSetup", false);

    QDBusMessage message = QDBusMessage::createMethodCall(s_login1Service, s_login1Path, s_dbusPropertiesInterface, QStringLiteral("Get"));
    message.setArguments({s_login1ManagerInterface, s_login1RebootToFirmwareSetup});
    QDBusPendingReply<QVariant> call = QDBusConnection::systemBus().asyncCall(message);
    QDBusPendingCallWatcher *callWatcher = new QDBusPendingCallWatcher(call, this);
    connect(callWatcher, &QDBusPendingCallWatcher::finished, context, [context](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QVariant> reply = *watcher;
        watcher->deleteLater();

        if (reply.value().toBool()) {
            context->setContextProperty("rebootToFirmwareSetup", true);
        }
    });

    // TODO KF6 remove, used to read "BootManager" from kdmrc
    context->setContextProperty(QStringLiteral("bootManager"), QStringLiteral("None"));

    // TODO KF6 remove. Unused
    context->setContextProperty(QStringLiteral("choose"), false);

    // TODO KF6 remove, used to call KDisplayManager::bootOptions
    QStringList rebootOptions;
    int def = 0;
    QQmlPropertyMap *rebootOptionsMap = new QQmlPropertyMap(this);
    rebootOptionsMap->insert(QStringLiteral("options"), QVariant::fromValue(rebootOptions));
    rebootOptionsMap->insert(QStringLiteral("default"), QVariant::fromValue(def));
    context->setContextProperty(QStringLiteral("rebootOptions"), rebootOptionsMap);

    // engine stuff
    KDeclarative::KDeclarative kdeclarative;
    kdeclarative.setDeclarativeEngine(engine());
    kdeclarative.setupEngine(engine());
    engine()->rootContext()->setContextObject(new KLocalizedContext(engine()));
}

void KSMShutdownDlg::init()
{
    rootContext()->setContextProperty(QStringLiteral("screenGeometry"), screen()->geometry());

    QString fileName;
    KPackage::Package package = KPackage::PackageLoader::self()->loadPackage(QStringLiteral("Plasma/LookAndFeel"));
    KConfigGroup cg(KSharedConfig::openConfig(QStringLiteral("kdeglobals")), "KDE");
    const QString packageName = cg.readEntry("LookAndFeelPackage", QString());
    if (!packageName.isEmpty()) {
        package.setPath(packageName);
    }

    fileName = package.filePath("logoutmainscript");

    if (QFile::exists(fileName)) {
        setSource(package.fileUrl("logoutmainscript"));
    } else {
        qCWarning(LOGOUT_GREETER) << "Couldn't find a theme for the Shutdown dialog" << fileName;
        return;
    }

    if (!errors().isEmpty()) {
        qCWarning(LOGOUT_GREETER) << errors();
    }

    connect(rootObject(), SIGNAL(logoutRequested()), SLOT(slotLogout()));
    connect(rootObject(), SIGNAL(haltRequested()), SLOT(slotHalt()));
    connect(rootObject(), SIGNAL(suspendRequested(int)), SLOT(slotSuspend(int)));
    connect(rootObject(), SIGNAL(rebootRequested()), SLOT(slotReboot()));
    connect(rootObject(), SIGNAL(rebootRequested2(int)), SLOT(slotReboot(int)));
    connect(rootObject(), SIGNAL(cancelRequested()), SLOT(reject()));
    connect(rootObject(), SIGNAL(lockScreenRequested()), SLOT(slotLockScreen()));

    connect(screen(), &QScreen::geometryChanged, this, [this] {
        setGeometry(screen()->geometry());
    });

    // decide in backgroundcontrast whether doing things darker or lighter
    // set backgroundcontrast here, because in QEvent::PlatformSurface
    // is too early and we don't have the root object yet
    const QColor backgroundColor = rootObject() ? rootObject()->property("backgroundColor").value<QColor>() : QColor();
    KWindowEffects::enableBackgroundContrast(winId(), true, 0.4, (backgroundColor.value() > 128 ? 1.6 : 0.3), 1.7);
    KQuickAddons::QuickViewSharedEngine::showFullScreen();
    setFlag(Qt::FramelessWindowHint);
    requestActivate();

    KWindowSystem::setState(winId(), NET::SkipTaskbar | NET::SkipPager);

    setKeyboardGrabEnabled(true);
}

void KSMShutdownDlg::resizeEvent(QResizeEvent *e)
{
    KQuickAddons::QuickViewSharedEngine::resizeEvent(e);

    if (KWindowSystem::compositingActive()) {
        // TODO: reenable window mask when we are without composite?
        //        clearMask();
    } else {
        //        setMask(m_view->mask());
    }
}

bool KSMShutdownDlg::event(QEvent *e)
{
    if (e->type() == QEvent::PlatformSurface) {
        switch (static_cast<QPlatformSurfaceEvent *>(e)->surfaceEventType()) {
        case QPlatformSurfaceEvent::SurfaceCreated:
            setupWaylandIntegration();
            KWindowEffects::enableBlurBehind(winId(), true);
            break;
        case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
            delete m_shellSurface;
            m_shellSurface = nullptr;
            break;
        }
    }
    return KQuickAddons::QuickViewSharedEngine::event(e);
}

void KSMShutdownDlg::setupWaylandIntegration()
{
    if (m_shellSurface) {
        // already setup
        return;
    }
    if (!m_waylandPlasmaShell) {
        return;
    }
    using namespace KWayland::Client;
    Surface *s = Surface::fromWindow(this);
    if (!s) {
        return;
    }

    m_shellSurface = m_waylandPlasmaShell->createSurface(s, this);
    // Use Role::Panel to make it go above all other windows
    // see allso KSplash splashwindow.cpp
    m_shellSurface->setPosition(geometry().topLeft());
    m_shellSurface->setRole(PlasmaShellSurface::Role::Panel);
    m_shellSurface->setPanelTakesFocus(true);
    m_shellSurface->setPanelBehavior(PlasmaShellSurface::PanelBehavior::WindowsGoBelow);
}

void KSMShutdownDlg::slotLogout()
{
    m_session.requestLogout(SessionManagement::ConfirmationMode::Skip);
    accept();
}

void KSMShutdownDlg::slotReboot()
{
    // no boot option selected -> current
    m_bootOption.clear();
    m_session.requestReboot(SessionManagement::ConfirmationMode::Skip);
    accept();
}

void KSMShutdownDlg::slotReboot(int opt)
{
    if (int(rebootOptions.size()) > opt)
        m_bootOption = rebootOptions[opt];
    m_session.requestReboot(SessionManagement::ConfirmationMode::Skip);
    accept();
}

void KSMShutdownDlg::slotLockScreen()
{
    m_bootOption.clear();
    m_session.lock();
    reject();
}

void KSMShutdownDlg::slotHalt()
{
    m_bootOption.clear();
    m_session.requestShutdown(SessionManagement::ConfirmationMode::Skip);
    accept();
}

void KSMShutdownDlg::slotSuspend(int spdMethod)
{
    m_bootOption.clear();
    switch (spdMethod) {
    case 1: // Solid::PowerManagement::StandbyState:
    case 2: // Solid::PowerManagement::SuspendState:
        m_session.suspend();
        break;
    case 4: // Solid::PowerManagement::HibernateState:
        m_session.hibernate();
        break;
    }
    reject();
}

void KSMShutdownDlg::accept()
{
    emit accepted();
}

void KSMShutdownDlg::reject()
{
    emit rejected();
}

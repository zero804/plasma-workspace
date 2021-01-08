/*****************************************************************
ksmserver - the KDE session management server

Copyright 2016 Martin Graesslin <mgraesslin@kde.org>

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
#include <QApplication>
#include <QCommandLineParser>
#include <QLibraryInfo>
#include <QQuickWindow>

#include <KQuickAddons/QtQuickSettings>

#include "ksmserveriface.h"

#include "greeter.h"

#include <sessionmanagement.h>

int main(int argc, char *argv[])
{
    qunsetenv("SESSION_MANAGER");

    KWorkSpace::detectPlatform(argc, argv);
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app(argc, argv);

    KQuickAddons::QtQuickSettings::init();

    OrgKdeKSMServerInterfaceInterface ksmserver(QStringLiteral("org.kde.ksmserver"), QStringLiteral("/KSMServer"), QDBusConnection::sessionBus());
    QDBusPendingReply<bool> isShuttingDownPending = ksmserver.isShuttingDown();

    isShuttingDownPending.waitForFinished();

    // if ksmserver is shutting us down already, we don't want another prompt
    if (isShuttingDownPending.value()) {
        return 0;
    }

    // because we export stuff as horrific contextProperties we need to know "maysd" may shutdown, at the time of initial creation and can't update
    // later.
    // Force the backend to load everything now, then the shared backend will be cached when a new object is created later

    // TODO Plasma 6, just have the greeter QML import and use the SessionManagement object directly
    // We don't need any special slot handling in ShutdownDlg
    SessionManagement m_session;
    if (m_session.state() == SessionManagement::State::Loading) {
        QEventLoop e;
        QObject::connect(&m_session, &SessionManagement::stateChanged, &e, &QEventLoop::quit);
        e.exec();
    }

    Greeter greeter;

    if (argc > 1) {
        // special case, invoked from ksmserver from a former release which had a tonne of args
        // shouldn't happen often
        greeter.promptLogout();
    }

    return app.exec();
}

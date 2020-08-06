/*
   Copyright (C) 2020 David Edmundson <davidedmundson@kde.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the Lesser GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

   You should have received a copy of the Lesser GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "qtclipboard.h"

#include <QClipboard>
#include <QGuiApplication>

QtClipboard::QtClipboard(QObject *parent)
    : SystemClipboard(parent)
{
    connect(qApp->clipboard(), &QClipboard::changed, this, &QtClipboard::changed);
}

void QtClipboard::setMimeData(QMimeData *mime, QClipboard::Mode mode)
{
    qApp->clipboard()->setMimeData(mime, mode);
}

void QtClipboard::clear(QClipboard::Mode mode)
{
    qApp->clipboard()->clear(mode);
}

const QMimeData *QtClipboard::mimeData(QClipboard::Mode mode) const
{
    return qApp->clipboard()->mimeData(mode);
}

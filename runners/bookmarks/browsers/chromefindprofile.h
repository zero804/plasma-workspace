/*
 *   Copyright 2007 Glenn Ergeerts <glenn.ergeerts@telenet.be>
 *   Copyright 2012 Glenn Ergeerts <marco.gulino@gmail.com>
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

#ifndef CHROMEFINDPROFILE_H
#define CHROMEFINDPROFILE_H

#include "browsers/findprofile.h"
#include <QDir>
#include <QObject>

class FindChromeProfile : public QObject, public FindProfile
{
public:
    explicit FindChromeProfile(const QString &applicationName, const QString &homeDirectory = QDir::homePath(), QObject *parent = nullptr);
    QList<Profile> find() override;

private:
    QString const m_applicationName;
    QString const m_homeDirectory;
};

#endif // CHROMEFINDPROFILE_H

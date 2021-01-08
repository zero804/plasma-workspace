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

#include "testchromebookmarks.h"
#include "browsers/chrome.h"
#include "browsers/chromefindprofile.h"
#include "favicon.h"
#include <QDir>
#include <QTest>

using namespace Plasma;

void TestChromeBookmarks::initTestCase()
{
    m_findBookmarksInCurrentDirectory.reset(
        new FakeFindProfile(QList<Profile>({Profile("chrome-config-home/Chrome-Bookmarks-Sample.json", "Sample", new FallbackFavicon())})));
}

void TestChromeBookmarks::bookmarkFinderShouldFindEachProfileDirectory()
{
    FindChromeProfile findChrome("chromium", "./chrome-config-home");
    QString profileTemplate = QStringLiteral("./chrome-config-home/.config/%1/%2/Bookmarks");

    QList<Profile> profiles = findChrome.find();
    QCOMPARE(profiles.size(), 2);
    QCOMPARE(profiles[0].path(), profileTemplate.arg("chromium", "Default"));
    QCOMPARE(profiles[1].path(), profileTemplate.arg("chromium", "Profile 1"));
}

void TestChromeBookmarks::bookmarkFinderShouldReportNoProfilesOnErrors()
{
    FindChromeProfile findChrome("chromium", "./no-config-directory");

    QList<Profile> profiles = findChrome.find();
    QCOMPARE(profiles.size(), 0);
}

void TestChromeBookmarks::itShouldFindNothingWhenPrepareIsNotCalled()
{
    Chrome *chrome = new Chrome(m_findBookmarksInCurrentDirectory.data(), this);
    QCOMPARE(chrome->match("any", true).size(), 0);
}

void TestChromeBookmarks::itShouldGracefullyExitWhenFileIsNotFound()
{
    FakeFindProfile finder(QList<Profile>() << Profile("FileNotExisting.json", QString(), nullptr));
    Chrome *chrome = new Chrome(&finder, this);
    chrome->prepare();
    QCOMPARE(chrome->match("any", true).size(), 0);
}

void verifyMatch(BookmarkMatch &match, const QString &title, const QString &url, qreal relevance, QueryMatch::Type type)
{
    QueryMatch queryMatch = match.asQueryMatch(nullptr);
    QCOMPARE(queryMatch.text(), title);
    QCOMPARE(queryMatch.data().toString(), url);
    QCOMPARE(queryMatch.relevance(), relevance);
    QVERIFY2(queryMatch.type() == type, QStringLiteral("Wrong query match type: expecting %1 but was %2").arg(type, queryMatch.type()).toLatin1());
}

void TestChromeBookmarks::itShouldFindAllBookmarks()
{
    Chrome *chrome = new Chrome(m_findBookmarksInCurrentDirectory.data(), this);
    chrome->prepare();
    QList<BookmarkMatch> matches = chrome->match("any", true);
    QCOMPARE(matches.size(), 3);
    verifyMatch(matches[0], "some bookmark in bookmark bar", "https://somehost.com/", 0.18, QueryMatch::PossibleMatch);
    verifyMatch(matches[1], "bookmark in other bookmarks", "https://otherbookmarks.com/", 0.18, QueryMatch::PossibleMatch);
    verifyMatch(matches[2], "bookmark in somefolder", "https://somefolder.com/", 0.18, QueryMatch::PossibleMatch);
}

void TestChromeBookmarks::itShouldFindOnlyMatches()
{
    Chrome *chrome = new Chrome(m_findBookmarksInCurrentDirectory.data(), this);
    chrome->prepare();
    QList<BookmarkMatch> matches = chrome->match("other", false);
    QCOMPARE(matches.size(), 1);
    verifyMatch(matches[0], "bookmark in other bookmarks", "https://otherbookmarks.com/", 0.45, QueryMatch::PossibleMatch);
}

void TestChromeBookmarks::itShouldClearResultAfterCallingTeardown()
{
    Chrome *chrome = new Chrome(m_findBookmarksInCurrentDirectory.data(), this);
    chrome->prepare();
    QCOMPARE(chrome->match("any", true).size(), 3);
    chrome->teardown();
    QCOMPARE(chrome->match("any", true).size(), 0);
}

void TestChromeBookmarks::itShouldFindBookmarksFromAllProfiles()
{
    FakeFindProfile findBookmarksFromAllProfiles(
        QList<Profile>() << Profile("chrome-config-home/Chrome-Bookmarks-Sample.json", "Sample", new FallbackFavicon(this))
                         << Profile("chrome-config-home/Chrome-Bookmarks-SecondProfile.json", "SecondProfile", new FallbackFavicon(this)));
    Chrome *chrome = new Chrome(&findBookmarksFromAllProfiles, this);
    chrome->prepare();
    QList<BookmarkMatch> matches = chrome->match("any", true);
    QCOMPARE(matches.size(), 4);
    verifyMatch(matches[0], "some bookmark in bookmark bar", "https://somehost.com/", 0.18, QueryMatch::PossibleMatch);
    verifyMatch(matches[1], "bookmark in other bookmarks", "https://otherbookmarks.com/", 0.18, QueryMatch::PossibleMatch);
    verifyMatch(matches[2], "bookmark in somefolder", "https://somefolder.com/", 0.18, QueryMatch::PossibleMatch);
    verifyMatch(matches[3], "bookmark in secondProfile", "https://secondprofile.com/", 0.18, QueryMatch::PossibleMatch);
}

QTEST_MAIN(TestChromeBookmarks);

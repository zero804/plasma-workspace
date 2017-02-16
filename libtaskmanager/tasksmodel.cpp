/********************************************************************
Copyright 2016  Eike Hein <hein@kde.org>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) version 3, or any
later version accepted by the membership of KDE e.V. (or its
successor approved by the membership of KDE e.V.), which shall
act as a proxy defined in Section 6 of version 3 of the license.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

#include "tasksmodel.h"
#include "activityinfo.h"
#include "concatenatetasksproxymodel.h"
#include "flattentaskgroupsproxymodel.h"
#include "taskfilterproxymodel.h"
#include "taskgroupingproxymodel.h"
#include "tasktools.h"

#include "launchertasksmodel.h"
#include "startuptasksmodel.h"
#include "windowtasksmodel.h"

#include "launchertasksmodel_p.h"

#include <QGuiApplication>
#include <QTimer>
#include <QUrl>

#include <numeric>

namespace TaskManager
{

class TasksModel::Private
{
public:
    Private(TasksModel *q);
    ~Private();

    static int instanceCount;

    static WindowTasksModel* windowTasksModel;
    static StartupTasksModel* startupTasksModel;
    LauncherTasksModel* launcherTasksModel = nullptr;
    ConcatenateTasksProxyModel* concatProxyModel = nullptr;
    TaskFilterProxyModel* filterProxyModel = nullptr;
    TaskGroupingProxyModel* groupingProxyModel = nullptr;
    FlattenTaskGroupsProxyModel* flattenGroupsProxyModel = nullptr;
    AbstractTasksModelIface *abstractTasksSourceModel = nullptr;

    bool anyTaskDemandsAttention = false;

    int launcherCount = 0;
    int virtualDesktop = -1;
    int screen = -1;
    QString activity;

    SortMode sortMode = SortAlpha;
    bool separateLaunchers = true;
    bool launchInPlace = false;
    bool launchersEverSet = false;
    bool launcherSortingDirty = false;
    bool launcherCheckNeeded = false;
    QList<int> sortedPreFilterRows;
    QVector<int> sortRowInsertQueue;
    QHash<QString, int> activityTaskCounts;
    static ActivityInfo* activityInfo;
    static int activityInfoUsers;

    bool groupInline = false;
    int groupingWindowTasksThreshold = -1;

    void initModels();
    void initLauncherTasksModel();
    void updateAnyTaskDemandsAttention();
    void updateManualSortMap();
    void consolidateManualSortMapForGroup(const QModelIndex &groupingProxyIndex);
    void updateGroupInline();
    QModelIndex preFilterIndex(const QModelIndex &sourceIndex) const;
    void updateActivityTaskCounts();
    void forceResort();
    bool lessThan(const QModelIndex &left, const QModelIndex &right,
        bool sortOnlyLaunchers = false) const;

private:
    TasksModel *q;
};

class TasksModel::TasksModelLessThan
{
public:
    inline TasksModelLessThan(const QAbstractItemModel *s, TasksModel *p, bool sortOnlyLaunchers)
        : sourceModel(s), tasksModel(p), sortOnlyLaunchers(sortOnlyLaunchers) {}

    inline bool operator()(int r1, int r2) const
    {
        QModelIndex i1 = sourceModel->index(r1, 0);
        QModelIndex i2 = sourceModel->index(r2, 0);
        return tasksModel->d->lessThan(i1, i2, sortOnlyLaunchers);
    }

private:
    const QAbstractItemModel *sourceModel;
    const TasksModel *tasksModel;
    bool sortOnlyLaunchers;
};

int TasksModel::Private::instanceCount = 0;
WindowTasksModel* TasksModel::Private::windowTasksModel = nullptr;
StartupTasksModel* TasksModel::Private::startupTasksModel = nullptr;
ActivityInfo* TasksModel::Private::activityInfo = nullptr;
int TasksModel::Private::activityInfoUsers = 0;

TasksModel::Private::Private(TasksModel *q)
    : q(q)
{
    ++instanceCount;
}

TasksModel::Private::~Private()
{
    --instanceCount;

    if (sortMode == SortActivity) {
        --activityInfoUsers;
    }

    if (!instanceCount) {
        delete windowTasksModel;
        windowTasksModel = nullptr;
        delete startupTasksModel;
        startupTasksModel = nullptr;
        delete activityInfo;
        activityInfo = nullptr;
    }
}

void TasksModel::Private::initModels()
{
    // NOTE: Overview over the entire model chain assembled here:
    // WindowTasksModel, StartupTasksModel, LauncherTasksModel
    //  -> concatProxyModel concatenates them into a single list.
    //   -> filterProxyModel filters by state (e.g. virtual desktop).
    //    -> groupingProxyModel groups by application (we go from flat list to tree).
    //     -> flattenGroupsProxyModel (optionally, if groupInline == true) flattens groups out.
    //      -> TasksModel collapses (top-level) items into task lifecycle abstraction; sorts.

    if (!windowTasksModel) {
        windowTasksModel = new WindowTasksModel();
    }

    QObject::connect(windowTasksModel, &QAbstractItemModel::rowsInserted, q,
        [this]() {
            if (sortMode == SortActivity) {
                updateActivityTaskCounts();
            }
        }
    );

    QObject::connect(windowTasksModel, &QAbstractItemModel::rowsRemoved, q,
        [this]() {
            if (sortMode == SortActivity) {
                updateActivityTaskCounts();
                forceResort();
            }
        }
    );

    QObject::connect(windowTasksModel, &QAbstractItemModel::dataChanged, q,
        [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles) {
            Q_UNUSED(topLeft)
            Q_UNUSED(bottomRight)

            if (sortMode == SortActivity && roles.contains(AbstractTasksModel::Activities)) {
                updateActivityTaskCounts();
            }

            if (roles.contains(AbstractTasksModel::IsActive)) {
                emit q->activeTaskChanged();
            }
        }
    );

    if (!startupTasksModel) {
        startupTasksModel = new StartupTasksModel();
    }

    concatProxyModel = new ConcatenateTasksProxyModel(q);

    concatProxyModel->addSourceModel(windowTasksModel);
    concatProxyModel->addSourceModel(startupTasksModel);

    // If we're in manual sort mode, we need to seed the sort map on pending row
    // insertions.
    QObject::connect(concatProxyModel, &QAbstractItemModel::rowsAboutToBeInserted, q,
        [this](const QModelIndex &parent, int start, int end) {
            Q_UNUSED(parent)

            if (sortMode != SortManual) {
                return;
            }

            const int delta = (end - start) + 1;
            QMutableListIterator<int> it(sortedPreFilterRows);

            while (it.hasNext()) {
                it.next();

                if (it.value() >= start) {
                    it.setValue(it.value() + delta);
                }
            }

            for (int i = start; i <= end; ++i) {
                sortedPreFilterRows.append(i);

                if (!separateLaunchers) {
                    sortRowInsertQueue.append(sortedPreFilterRows.count() - 1);
                }
            }
        }
    );

    // If we're in manual sort mode, we need to update the sort map on row insertions.
    QObject::connect(concatProxyModel, &QAbstractItemModel::rowsInserted, q,
        [this](const QModelIndex &parent, int start, int end) {
            Q_UNUSED(parent)
            Q_UNUSED(start)
            Q_UNUSED(end)

            if (sortMode == SortManual) {
                updateManualSortMap();
            }
        }
    );

    // If we're in manual sort mode, we need to update the sort map after row removals.
    QObject::connect(concatProxyModel, &QAbstractItemModel::rowsRemoved, q,
        [this](const QModelIndex &parent, int first, int last) {
            Q_UNUSED(parent)

            if (sortMode != SortManual) {
                return;
            }

            for (int i = first; i <= last; ++i) {
                sortedPreFilterRows.removeOne(i);
            }

            const int delta = (last - first) + 1;
            QMutableListIterator<int> it(sortedPreFilterRows);

            while (it.hasNext()) {
                it.next();

                if (it.value() > last) {
                    it.setValue(it.value() - delta);
                }
            }
        }
    );

    filterProxyModel = new TaskFilterProxyModel(q);
    filterProxyModel->setSourceModel(concatProxyModel);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::virtualDesktopChanged,
        q, &TasksModel::virtualDesktopChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::screenGeometryChanged,
        q, &TasksModel::screenGeometryChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::activityChanged,
        q, &TasksModel::activityChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::filterByVirtualDesktopChanged,
        q, &TasksModel::filterByVirtualDesktopChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::filterByScreenChanged,
        q, &TasksModel::filterByScreenChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::filterByActivityChanged,
        q, &TasksModel::filterByActivityChanged);
    QObject::connect(filterProxyModel, &TaskFilterProxyModel::filterNotMinimizedChanged,
        q, &TasksModel::filterNotMinimizedChanged);

    groupingProxyModel = new TaskGroupingProxyModel(q);
    groupingProxyModel->setSourceModel(filterProxyModel);
    QObject::connect(groupingProxyModel, &TaskGroupingProxyModel::groupModeChanged,
        q, &TasksModel::groupModeChanged);
    QObject::connect(groupingProxyModel, &TaskGroupingProxyModel::blacklistedAppIdsChanged,
        q, &TasksModel::groupingAppIdBlacklistChanged);
    QObject::connect(groupingProxyModel, &TaskGroupingProxyModel::blacklistedLauncherUrlsChanged,
        q, &TasksModel::groupingLauncherUrlBlacklistChanged);

    QObject::connect(groupingProxyModel, &QAbstractItemModel::rowsInserted, q,
        [this](const QModelIndex &parent, int first, int last) {
            if (parent.isValid()) {
                if (sortMode == SortManual) {
                    consolidateManualSortMapForGroup(parent);
                }

                // Existence of a group means everything below this has already been done.
                return;
            }

            for (int i = first; i <= last; ++i) {
                const QModelIndex &sourceIndex = groupingProxyModel->index(i, 0);
                const QString &appId = sourceIndex.data(AbstractTasksModel::AppId).toString();

                if (sourceIndex.data(AbstractTasksModel::IsDemandingAttention).toBool()) {
                    updateAnyTaskDemandsAttention();
                }

                // When we get a window we have a startup for, cause the startup to be re-filtered.
                if (sourceIndex.data(AbstractTasksModel::IsWindow).toBool()) {
                    const QString &appName = sourceIndex.data(AbstractTasksModel::AppName).toString();

                    for (int i = 0; i < filterProxyModel->rowCount(); ++i) {
                        QModelIndex filterIndex = filterProxyModel->index(i, 0);

                        if (!filterIndex.data(AbstractTasksModel::IsStartup).toBool()) {
                            continue;
                        }

                        if ((!appId.isEmpty() && appId == filterIndex.data(AbstractTasksModel::AppId).toString())
                            || (!appName.isEmpty() && appName == filterIndex.data(AbstractTasksModel::AppName).toString())) {
                            filterProxyModel->dataChanged(filterIndex, filterIndex);
                        }
                    }
                }

                // When we get a window or startup we have a launcher for, cause the launcher to be re-filtered.
                if (sourceIndex.data(AbstractTasksModel::IsWindow).toBool()
                    || sourceIndex.data(AbstractTasksModel::IsStartup).toBool()) {
                    for (int i = 0; i < filterProxyModel->rowCount(); ++i) {
                        const QModelIndex &filterIndex = filterProxyModel->index(i, 0);

                        if (!filterIndex.data(AbstractTasksModel::IsLauncher).toBool()) {
                            continue;
                        }

                        if (appsMatch(sourceIndex, filterIndex)) {
                            filterProxyModel->dataChanged(filterIndex, filterIndex);
                        }
                    }
                }
            }
         }
    );

    QObject::connect(groupingProxyModel, &QAbstractItemModel::rowsAboutToBeRemoved, q,
        [this](const QModelIndex &parent, int first, int last) {
            // We can ignore group members.
            if (parent.isValid()) {
                return;
            }

            for (int i = first; i <= last; ++i) {
                const QModelIndex &sourceIndex = groupingProxyModel->index(i, 0);

                if (sourceIndex.data(AbstractTasksModel::IsDemandingAttention).toBool()) {
                    updateAnyTaskDemandsAttention();
                }

                // When a window or startup task is removed, we have to trigger a re-filter of
                // our launchers to (possibly) pop them back in.
                // NOTE: An older revision of this code compared the window and startup tasks
                // to the launchers to figure out which launchers should be re-filtered. This
                // was fine until we discovered that certain applications (e.g. Google Chrome)
                // change their window metadata specifically during tear-down, sometimes
                // breaking TaskTools::appsMatch (it's a race) and causing the associated
                // launcher to remain hidden. Therefore we now consider any top-level window or
                // startup task removal a trigger to re-filter all launchers. We don't do this
                // in response to the window metadata changes (even though it would be strictly
                // more correct, as then-ending identity match-up was what caused the launcher
                // to be hidden) because we don't want the launcher and window/startup task to
                // briefly co-exist in the model.
                if (!launcherCheckNeeded
                      && launcherTasksModel
                      && (sourceIndex.data(AbstractTasksModel::IsWindow).toBool()
                      || sourceIndex.data(AbstractTasksModel::IsStartup).toBool())) {
                    launcherCheckNeeded = true;
                }
            }
        }
    );

    QObject::connect(filterProxyModel, &QAbstractItemModel::rowsRemoved, q,
        [this](const QModelIndex &parent, int first, int last) {
            Q_UNUSED(parent)
            Q_UNUSED(first)
            Q_UNUSED(last)

            if (launcherCheckNeeded) {
                QMetaObject::invokeMethod(launcherTasksModel, "dataChanged",
                    Q_ARG(QModelIndex, launcherTasksModel->index(0, 0)),
                    Q_ARG(QModelIndex, launcherTasksModel->index(launcherTasksModel->rowCount() - 1, 0)));

                launcherCheckNeeded = false;
            }
        }
    );

    // Update anyTaskDemandsAttention on source data changes.
    QObject::connect(groupingProxyModel, &QAbstractItemModel::dataChanged, q,
        [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles) {
            Q_UNUSED(bottomRight)

            // We can ignore group members.
            if (topLeft.parent().isValid()) {
                return;
            }

            if (roles.isEmpty() || roles.contains(AbstractTasksModel::IsDemandingAttention)) {
                updateAnyTaskDemandsAttention();
            }
        }
    );

    // Update anyTaskDemandsAttention on source model resets.
    QObject::connect(groupingProxyModel, &QAbstractItemModel::modelReset, q,
        [this]() { updateAnyTaskDemandsAttention(); }
    );

    abstractTasksSourceModel = groupingProxyModel;
    q->setSourceModel(groupingProxyModel);

    QObject::connect(q, &QAbstractItemModel::rowsInserted, q, &TasksModel::updateLauncherCount);
    QObject::connect(q, &QAbstractItemModel::rowsRemoved, q, &TasksModel::updateLauncherCount);
    QObject::connect(q, &QAbstractItemModel::modelReset, q, &TasksModel::updateLauncherCount);

    QObject::connect(q, &QAbstractItemModel::rowsInserted, q, &TasksModel::countChanged);
    QObject::connect(q, &QAbstractItemModel::rowsRemoved, q, &TasksModel::countChanged);
    QObject::connect(q, &QAbstractItemModel::modelReset, q, &TasksModel::countChanged);
}

void TasksModel::Private::updateAnyTaskDemandsAttention()
{
    bool taskFound = false;

    for (int i = 0; i < groupingProxyModel->rowCount(); ++i) {
        if (groupingProxyModel->index(i, 0).data(AbstractTasksModel::IsDemandingAttention).toBool()) {
            taskFound = true;
            break;
        }
    }

    if (taskFound != anyTaskDemandsAttention) {
        anyTaskDemandsAttention = taskFound;
        q->anyTaskDemandsAttentionChanged();
    }
}

void TasksModel::Private::initLauncherTasksModel()
{
    if (launcherTasksModel) {
        return;
    }

    launcherTasksModel = new LauncherTasksModel(q);
    QObject::connect(launcherTasksModel, &LauncherTasksModel::launcherListChanged,
        q, &TasksModel::launcherListChanged);
    QObject::connect(launcherTasksModel, &LauncherTasksModel::launcherListChanged,
        q, &TasksModel::updateLauncherCount);

    concatProxyModel->addSourceModel(launcherTasksModel);
}

void TasksModel::Private::updateManualSortMap()
{
    // Empty map; full sort.
    if (sortedPreFilterRows.isEmpty()) {
        sortedPreFilterRows.reserve(concatProxyModel->rowCount());

        for (int i = 0; i < concatProxyModel->rowCount(); ++i) {
            sortedPreFilterRows.append(i);
        }

        // Full sort.
        TasksModelLessThan lt(concatProxyModel, q, false);
        std::stable_sort(sortedPreFilterRows.begin(), sortedPreFilterRows.end(), lt);

        // Consolidate sort map entries for groups.
        if (q->groupMode() != GroupDisabled) {
            for (int i = 0; i < groupingProxyModel->rowCount(); ++i) {
                const QModelIndex &groupingIndex = groupingProxyModel->index(i, 0);

                if (groupingIndex.data(AbstractTasksModel::IsGroupParent).toBool()) {
                    consolidateManualSortMapForGroup(groupingIndex);
                }
            }
        }

        return;
    }

    // Existing map; check whether launchers need sorting by launcher list position.
    if (separateLaunchers) {
        // Sort only launchers.
        TasksModelLessThan lt(concatProxyModel, q, true);
        std::stable_sort(sortedPreFilterRows.begin(), sortedPreFilterRows.end(), lt);
    // Otherwise process any entries in the insert queue and move them intelligently
    // in the sort map.
    } else {
         while (sortRowInsertQueue.count()) {
             const int row = sortRowInsertQueue.takeFirst();
             const QModelIndex &idx = concatProxyModel->index(sortedPreFilterRows.at(row), 0);

             bool moved = false;

             // Try to move the task up to its right-most app sibling, unless this
             // is us sorting in a launcher list for the first time.
             if (launchersEverSet && !idx.data(AbstractTasksModel::IsLauncher).toBool()) {
                 for (int i = (row - 1); i >= 0; --i) {
                     const QModelIndex &concatProxyIndex = concatProxyModel->index(sortedPreFilterRows.at(i), 0);

                     if (appsMatch(concatProxyIndex, idx)) {
                         sortedPreFilterRows.move(row, i + 1);
                         moved = true;

                         break;
                     }
                 }
             }

             int insertPos = 0;

             // If unsuccessful or skipped, and the new task is a launcher, put after
             // the rightmost launcher or launcher-backed task in the map, or failing
             // that at the start of the map.
             if (!moved && idx.data(AbstractTasksModel::IsLauncher).toBool()) {
                 for (int i = 0; i < row; ++i) {
                     const QModelIndex &concatProxyIndex = concatProxyModel->index(sortedPreFilterRows.at(i), 0);

                     if (concatProxyIndex.data(AbstractTasksModel::IsLauncher).toBool()
                        || launcherTasksModel->launcherPosition(concatProxyIndex.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl()) != -1) {
                        insertPos = i + 1;
                     } else {
                        break;
                     }
                 }

                 sortedPreFilterRows.move(row, insertPos);
                 moved = true;
             }

             // If we sorted in a launcher and it's the first time we're sorting in a
             // launcher list, move existing windows to the launcher position now.
             if (moved && !launchersEverSet) {
                 for (int i = (sortedPreFilterRows.count() - 1); i >= 0; --i) {
                     const QModelIndex &concatProxyIndex = concatProxyModel->index(sortedPreFilterRows.at(i), 0);

                     if (!concatProxyIndex.data(AbstractTasksModel::IsLauncher).toBool()
                         && idx.data(AbstractTasksModel::LauncherUrlWithoutIcon) == concatProxyIndex.data(AbstractTasksModel::LauncherUrlWithoutIcon)) {
                         sortedPreFilterRows.move(i, insertPos);

                         if (insertPos > i) {
                             --insertPos;
                         }
                     }
                 }
             }
         }
     }
 }

void TasksModel::Private::consolidateManualSortMapForGroup(const QModelIndex &groupingProxyIndex)
{
    // Consolidates sort map entries for a group's items to be contiguous
    // after the group's first item and the same order as in groupingProxyModel.

    const int childCount = groupingProxyModel->rowCount(groupingProxyIndex);

    if (!childCount) {
        return;
    }

    const QModelIndex &leader = groupingProxyIndex.child(0, 0);
    const QModelIndex &preFilterLeader = filterProxyModel->mapToSource(groupingProxyModel->mapToSource(leader));

    // We're moving the trailing children to the sort map position of
    // the first child, so we're skipping the first child.
    for (int i = 1; i < childCount; ++i) {
        const QModelIndex &child = groupingProxyIndex.child(i, 0);
        const QModelIndex &preFilterChild = filterProxyModel->mapToSource(groupingProxyModel->mapToSource(child));
        const int leaderPos = sortedPreFilterRows.indexOf(preFilterLeader.row());
        const int childPos = sortedPreFilterRows.indexOf(preFilterChild.row());
        const int insertPos = (leaderPos + i) + ((leaderPos + i) > childPos ? -1 : 0);
        sortedPreFilterRows.move(childPos, insertPos);
    }
}

void TasksModel::Private::updateGroupInline()
{
    if (q->groupMode() != GroupDisabled && groupInline) {
        if (flattenGroupsProxyModel) {
            return;
        }

        // Exempting tasks which demand attention from grouping is not
        // necessary when all group children are shown inline anyway
        // and would interfere with our sort-tasks-together goals.
        groupingProxyModel->setGroupDemandingAttention(true);

        // Likewise, ignore the window tasks threshold when making
        // grouping decisions.
        groupingProxyModel->setWindowTasksThreshold(-1);

        flattenGroupsProxyModel = new FlattenTaskGroupsProxyModel(q);
        flattenGroupsProxyModel->setSourceModel(groupingProxyModel);

        abstractTasksSourceModel = flattenGroupsProxyModel;
        q->setSourceModel(flattenGroupsProxyModel);

        if (sortMode == SortManual) {
            forceResort();
        }
    } else {
        if (!flattenGroupsProxyModel) {
            return;
        }

        groupingProxyModel->setGroupDemandingAttention(false);
        groupingProxyModel->setWindowTasksThreshold(groupingWindowTasksThreshold);

        abstractTasksSourceModel = groupingProxyModel;
        q->setSourceModel(groupingProxyModel);

        delete flattenGroupsProxyModel;
        flattenGroupsProxyModel = nullptr;

        if (sortMode == SortManual) {
            forceResort();
        }
    }
}

QModelIndex TasksModel::Private::preFilterIndex(const QModelIndex &sourceIndex) const {
    // Only in inline grouping mode, we have an additional proxy layer.
    if (flattenGroupsProxyModel) {
        return filterProxyModel->mapToSource(groupingProxyModel->mapToSource(flattenGroupsProxyModel->mapToSource(sourceIndex)));
    } else {
        return filterProxyModel->mapToSource(groupingProxyModel->mapToSource(sourceIndex));
    }
}

void TasksModel::Private::updateActivityTaskCounts()
{
    // Collects the number of window tasks on each activity.

    activityTaskCounts.clear();

    if (!windowTasksModel || !activityInfo) {
        return;
    }

    foreach(const QString &activity, activityInfo->runningActivities()) {
        activityTaskCounts.insert(activity, 0);
    }

    for (int i = 0; i < windowTasksModel->rowCount(); ++i) {
        const QModelIndex &windowIndex = windowTasksModel->index(i, 0);
        const QStringList &activities = windowIndex.data(AbstractTasksModel::Activities).toStringList();

        if (activities.isEmpty()) {
            QMutableHashIterator<QString, int> i(activityTaskCounts);

            while (i.hasNext()) {
                i.next();
                i.setValue(i.value() + 1);
            }
        } else {
            foreach(const QString &activity, activities) {
                ++activityTaskCounts[activity];
            }
        }
    }
}

void TasksModel::Private::forceResort()
{
    // HACK: This causes QSortFilterProxyModel to run all rows through
    // our lessThan() implementation again.
    q->setDynamicSortFilter(false);
    q->setDynamicSortFilter(true);
}

bool TasksModel::Private::lessThan(const QModelIndex &left, const QModelIndex &right, bool sortOnlyLaunchers) const
{
    // Launcher tasks go first.
    // When launchInPlace is enabled, startup and window tasks are sorted
    // as the launchers they replace (see also move()).

    if (separateLaunchers) {
        if (left.data(AbstractTasksModel::IsLauncher).toBool() && right.data(AbstractTasksModel::IsLauncher).toBool()) {
            return (left.row() < right.row());
        } else if (left.data(AbstractTasksModel::IsLauncher).toBool() && !right.data(AbstractTasksModel::IsLauncher).toBool()) {
            if (launchInPlace) {
                const int leftPos = q->launcherPosition(left.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());
                const int rightPos = q->launcherPosition(right.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());

                if (rightPos != -1) {
                    return (leftPos < rightPos);
                }
            }

            return true;
        } else if (!left.data(AbstractTasksModel::IsLauncher).toBool() && right.data(AbstractTasksModel::IsLauncher).toBool()) {
            if (launchInPlace) {
                const int leftPos = q->launcherPosition(left.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());
                const int rightPos = q->launcherPosition(right.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());

                if (leftPos != -1) {
                    return (leftPos < rightPos);
                }
            }

            return false;
        } else if (launchInPlace) {
            const int leftPos = q->launcherPosition(left.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());
            const int rightPos = q->launcherPosition(right.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());

            if (leftPos != -1 && rightPos != -1) {
                return (leftPos < rightPos);
            } else if (leftPos != -1 && rightPos == -1) {
                return true;
            } else if (leftPos == -1 && rightPos != -1) {
                return false;
            }
        }
    }

    // If told to stop after launchers we fall through to the existing map if it exists.
    if (sortOnlyLaunchers && !sortedPreFilterRows.isEmpty()) {
        return (sortedPreFilterRows.indexOf(left.row()) < sortedPreFilterRows.indexOf(right.row()));
    }

    // Sort other cases by sort mode.
    switch (sortMode) {
        case SortVirtualDesktop: {
            const QVariant &leftDesktopVariant = left.data(AbstractTasksModel::VirtualDesktop);
            bool leftOk = false;
            const int leftDesktop = leftDesktopVariant.toInt(&leftOk);

            const QVariant &rightDesktopVariant = right.data(AbstractTasksModel::VirtualDesktop);
            bool rightOk = false;
            const int rightDesktop = rightDesktopVariant.toInt(&rightOk);

            if (leftOk && rightOk && (leftDesktop != rightDesktop)) {
                return (leftDesktop < rightDesktop);
            } else if (leftOk && !rightOk) {
                return false;
            } else if (!leftOk && rightOk) {
                return true;
            }
        }
        case SortActivity: {
            // updateActivityTaskCounts() counts the number of window tasks on each
            // activity. This will sort tasks by comparing a cumulative score made
            // up of the task counts for each acvtivity a task is assigned to, and
            // otherwise fall through to alphabetical sorting.
            int leftScore = -1;
            int rightScore = -1;

            const QStringList &leftActivities = left.data(AbstractTasksModel::Activities).toStringList();

            if (!leftActivities.isEmpty()) {
                foreach(const QString& activity, leftActivities) {
                    leftScore += activityTaskCounts[activity];
                }
            }

            const QStringList &rightActivities = right.data(AbstractTasksModel::Activities).toStringList();

            if (!rightActivities.isEmpty()) {
                foreach(const QString& activity, rightActivities) {
                    rightScore += activityTaskCounts[activity];
                }
            }

            if (leftScore == -1 || rightScore == -1) {
                const QList<int> &counts = activityTaskCounts.values();
                const int sumScore = std::accumulate(counts.begin(), counts.end(), 0);

                if (leftScore == -1) {
                    leftScore = sumScore;
                }

                if (rightScore == -1) {
                    rightScore = sumScore;
                }
            }

            if (leftScore != rightScore) {
                return (leftScore > rightScore);
            }
        }
        // Fall through to source order if sorting is disabled or manual, or alphabetical by app name otherwise.
        default: {
            if (sortMode == SortDisabled) {
                return (left.row() < right.row());
            } else {
                // The overall goal of alphabetic sorting is to sort tasks belonging to the
                // same app together, while sorting the resulting sets alphabetically among
                // themselves by the app name. The following code tries to achieve this by
                // going for AppName first, and falling back to DisplayRole - which for
                // window-type tasks generally contains the window title - if AppName is
                // not available. When comparing tasks with identical resulting sort strings,
                // we sort them by the source model order (i.e. insertion/creation). Older
                // versions of this code compared tasks by a concatenation of AppName and
                // DisplayRole at all times, but always sorting by the window title does more
                // than our goal description - and can cause tasks within an app's set to move
                // around when window titles change, which is a nuisance for users (especially
                // in case of tabbed apps that have the window title reflect the active tab,
                // e.g. web browsers). To recap, the common case is "sort by AppName, then
                // insertion order", only swapping out AppName for DisplayRole (i.e. window
                // title) when necessary.

                QString leftSortString = left.data(AbstractTasksModel::AppName).toString();

                if (leftSortString.isEmpty()) {
                    leftSortString = left.data(Qt::DisplayRole).toString();
                }

                QString rightSortString = right.data(AbstractTasksModel::AppName).toString();

                const QString &rightSortString = right.data(AbstractTasksModel::AppName).toString()
                    + right.data(Qt::DisplayRole).toString();

                return (leftSortString.localeAwareCompare(rightSortString) < 0);
            }
        }
    }
}

TasksModel::TasksModel(QObject *parent)
    : QSortFilterProxyModel(parent)
    , d(new Private(this))
{
    d->initModels();

    // Start sorting.
    sort(0);
}

TasksModel::~TasksModel()
{
}

QHash<int, QByteArray> TasksModel::roleNames() const
{
    if (d->windowTasksModel) {
        return d->windowTasksModel->roleNames();
    }

    return QHash<int, QByteArray>();
}

int TasksModel::rowCount(const QModelIndex &parent) const
{
    return QSortFilterProxyModel::rowCount(parent);
}

QVariant TasksModel::data(const QModelIndex &proxyIndex, int role) const
{
    if (rowCount(proxyIndex) && role == AbstractTasksModel::LegacyWinIdList) {
        QVariantList winIds;

        for (int i = 0; i < rowCount(proxyIndex); ++i) {
            winIds.append(proxyIndex.child(i, 0).data(AbstractTasksModel::LegacyWinIdList).toList());
        }

        return winIds;
    }

    return QSortFilterProxyModel::data(proxyIndex, role);
}

void TasksModel::updateLauncherCount()
{
    if (!d->launcherTasksModel) {
        return;
    }

    int count = 0;

    for (int i = 0; i < rowCount(); ++i) {
        if (index(i, 0).data(AbstractTasksModel::IsLauncher).toBool()) {
            ++count;
        }
    }

    if (d->launcherCount != count) {
        d->launcherCount = count;
        emit launcherCountChanged();
    }
}

int TasksModel::launcherCount() const
{
    return d->launcherCount;
}

bool TasksModel::anyTaskDemandsAttention() const
{
    return d->anyTaskDemandsAttention;
}

int TasksModel::virtualDesktop() const
{
    return d->filterProxyModel->virtualDesktop();
}

void TasksModel::setVirtualDesktop(int virtualDesktop)
{
    d->filterProxyModel->setVirtualDesktop(virtualDesktop);
}

QRect TasksModel::screenGeometry() const
{
    return d->filterProxyModel->screenGeometry();
}

void TasksModel::setScreenGeometry(const QRect &geometry)
{
    d->filterProxyModel->setScreenGeometry(geometry);
}

QString TasksModel::activity() const
{
    return d->filterProxyModel->activity();
}

void TasksModel::setActivity(const QString &activity)
{
    d->filterProxyModel->setActivity(activity);
}

bool TasksModel::filterByVirtualDesktop() const
{
    return d->filterProxyModel->filterByVirtualDesktop();
}

void TasksModel::setFilterByVirtualDesktop(bool filter)
{
    d->filterProxyModel->setFilterByVirtualDesktop(filter);
}

bool TasksModel::filterByScreen() const
{
    return d->filterProxyModel->filterByScreen();
}

void TasksModel::setFilterByScreen(bool filter)
{
    d->filterProxyModel->setFilterByScreen(filter);
}

bool TasksModel::filterByActivity() const
{
    return d->filterProxyModel->filterByActivity();
}

void TasksModel::setFilterByActivity(bool filter)
{
    d->filterProxyModel->setFilterByActivity(filter);
}

bool TasksModel::filterNotMinimized() const
{
    return d->filterProxyModel->filterNotMinimized();
}

void TasksModel::setFilterNotMinimized(bool filter)
{
    d->filterProxyModel->setFilterNotMinimized(filter);
}

TasksModel::SortMode TasksModel::sortMode() const
{
    return d->sortMode;
}

void TasksModel::setSortMode(SortMode mode)
{
    if (d->sortMode != mode) {
        if (mode == SortManual) {
            d->updateManualSortMap();
        } else if (d->sortMode == SortManual) {
            d->sortedPreFilterRows.clear();
        }

        if (mode == SortActivity) {
            if (!d->activityInfo) {
                d->activityInfo = new ActivityInfo();
            }

            ++d->activityInfoUsers;

            d->updateActivityTaskCounts();
            setSortRole(AbstractTasksModel::Activities);
        } else if (d->sortMode == SortActivity) {
            --d->activityInfoUsers;

            if (!d->activityInfoUsers) {
                delete d->activityInfo;
                d->activityInfo = nullptr;
            }

            d->activityTaskCounts.clear();
            setSortRole(Qt::DisplayRole);
        }

        d->sortMode = mode;

        d->forceResort();

        emit sortModeChanged();
    }
}

bool TasksModel::separateLaunchers() const
{
    return d->separateLaunchers;
}

void TasksModel::setSeparateLaunchers(bool separate)
{
    if (d->separateLaunchers != separate) {
        d->separateLaunchers = separate;

        d->updateManualSortMap();
        d->forceResort();

        emit separateLaunchersChanged();
    }
}

bool TasksModel::launchInPlace() const
{
    return d->launchInPlace;
}

void TasksModel::setLaunchInPlace(bool launchInPlace)
{
    if (d->launchInPlace != launchInPlace) {
        d->launchInPlace = launchInPlace;

        d->forceResort();

        emit launchInPlaceChanged();
    }
}

TasksModel::GroupMode TasksModel::groupMode() const
{
    if (!d->groupingProxyModel) {
        return GroupDisabled;
    }

    return d->groupingProxyModel->groupMode();
}

void TasksModel::setGroupMode(GroupMode mode)
{
    if (d->groupingProxyModel) {
        if (mode == GroupDisabled && d->flattenGroupsProxyModel) {
            d->flattenGroupsProxyModel->setSourceModel(nullptr);
        }

        d->groupingProxyModel->setGroupMode(mode);
        d->updateGroupInline();
    }
}

bool TasksModel::groupInline() const
{
    return d->groupInline;
}

void TasksModel::setGroupInline(bool groupInline)
{
    if (d->groupInline != groupInline) {
        d->groupInline = groupInline;

        d->updateGroupInline();

        emit groupInlineChanged();
    }
}

int TasksModel::groupingWindowTasksThreshold() const
{
    return d->groupingWindowTasksThreshold;
}

void TasksModel::setGroupingWindowTasksThreshold(int threshold)
{
    if (d->groupingWindowTasksThreshold != threshold) {
        d->groupingWindowTasksThreshold = threshold;

        if (!d->groupInline && d->groupingProxyModel) {
            d->groupingProxyModel->setWindowTasksThreshold(threshold);
        }

        emit groupingWindowTasksThresholdChanged();
    }
}

QStringList TasksModel::groupingAppIdBlacklist() const
{
    if (!d->groupingProxyModel) {
        return QStringList();
    }

    return d->groupingProxyModel->blacklistedAppIds();
}

void TasksModel::setGroupingAppIdBlacklist(const QStringList &list)
{
    if (d->groupingProxyModel) {
        d->groupingProxyModel->setBlacklistedAppIds(list);
    }
}

QStringList TasksModel::groupingLauncherUrlBlacklist() const
{
    if (!d->groupingProxyModel) {
        return QStringList();
    }

    return d->groupingProxyModel->blacklistedLauncherUrls();
}

void TasksModel::setGroupingLauncherUrlBlacklist(const QStringList &list)
{
    if (d->groupingProxyModel) {
        d->groupingProxyModel->setBlacklistedLauncherUrls(list);
    }
}

QStringList TasksModel::launcherList() const
{
    if (d->launcherTasksModel) {
        return d->launcherTasksModel->launcherList();
    }

    return QStringList();
}

void TasksModel::setLauncherList(const QStringList &launchers)
{
    d->initLauncherTasksModel();
    d->launcherTasksModel->setLauncherList(launchers);
    d->launchersEverSet = true;
}

bool TasksModel::requestAddLauncher(const QUrl &url)
{
    d->initLauncherTasksModel();

    bool added = d->launcherTasksModel->requestAddLauncher(url);

    // If using manual and launch-in-place sorting with separate launchers,
    // we need to trigger a sort map update to move any window tasks to
    // their launcher position now.
    if (added && d->sortMode == SortManual && (d->launchInPlace || !d->separateLaunchers)) {
        d->updateManualSortMap();
        d->forceResort();
    }

    return added;
}

bool TasksModel::requestRemoveLauncher(const QUrl &url)
{
    if (d->launcherTasksModel) {
        bool removed = d->launcherTasksModel->requestRemoveLauncher(url);

        // If using manual and launch-in-place sorting with separate launchers,
        // we need to trigger a sort map update to move any window tasks no
        // longer backed by a launcher out of the launcher area.
        if (removed && d->sortMode == SortManual && (d->launchInPlace || !d->separateLaunchers)) {
            d->updateManualSortMap();
            d->forceResort();
        }

        return removed;
    }

    return false;
}

bool TasksModel::requestAddLauncherToActivity(const QUrl &url, const QString &activity)
{
    d->initLauncherTasksModel();

    bool added = d->launcherTasksModel->requestAddLauncherToActivity(url, activity);

    // If using manual and launch-in-place sorting with separate launchers,
    // we need to trigger a sort map update to move any window tasks to
    // their launcher position now.
    if (added && d->sortMode == SortManual && (d->launchInPlace || !d->separateLaunchers)) {
        d->updateManualSortMap();
        d->forceResort();
    }

    return added;
}

bool TasksModel::requestRemoveLauncherFromActivity(const QUrl &url, const QString &activity)
{
    if (d->launcherTasksModel) {
        bool removed = d->launcherTasksModel->requestRemoveLauncherFromActivity(url, activity);

        // If using manual and launch-in-place sorting with separate launchers,
        // we need to trigger a sort map update to move any window tasks no
        // longer backed by a launcher out of the launcher area.
        if (removed && d->sortMode == SortManual && (d->launchInPlace || !d->separateLaunchers)) {
            d->updateManualSortMap();
            d->forceResort();
        }

        return removed;
    }

    return false;
}

QStringList TasksModel::launcherActivities(const QUrl &url)
{
    if (d->launcherTasksModel) {
        return d->launcherTasksModel->launcherActivities(url);
    }

    return {};
}

int TasksModel::launcherPosition(const QUrl &url) const
{
    if (d->launcherTasksModel) {
        return d->launcherTasksModel->launcherPosition(url);
    }

    return -1;
}

void TasksModel::requestActivate(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestActivate(mapToSource(index));
    }
}

void TasksModel::requestNewInstance(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestNewInstance(mapToSource(index));
    }
}

void TasksModel::requestOpenUrls(const QModelIndex &index, const QList<QUrl> &urls)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestOpenUrls(mapToSource(index), urls);
    }
}

void TasksModel::requestClose(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestClose(mapToSource(index));
    }
}

void TasksModel::requestMove(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestMove(mapToSource(index));
    }
}

void TasksModel::requestResize(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestResize(mapToSource(index));
    }
}

void TasksModel::requestToggleMinimized(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleMinimized(mapToSource(index));
    }
}

void TasksModel::requestToggleMaximized(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleMaximized(mapToSource(index));
    }
}

void TasksModel::requestToggleKeepAbove(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleKeepAbove(mapToSource(index));
    }
}

void TasksModel::requestToggleKeepBelow(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleKeepBelow(mapToSource(index));
    }
}

void TasksModel::requestToggleFullScreen(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleFullScreen(mapToSource(index));
    }
}

void TasksModel::requestToggleShaded(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestToggleShaded(mapToSource(index));
    }
}

void TasksModel::requestVirtualDesktop(const QModelIndex &index, qint32 desktop)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestVirtualDesktop(mapToSource(index), desktop);
    }
}

void TasksModel::requestActivities(const QModelIndex &index, const QStringList &activities)
{
    if (index.isValid() && index.model() == this) {
        d->groupingProxyModel->requestActivities(mapToSource(index), activities);
    }
}

void TasksModel::requestPublishDelegateGeometry(const QModelIndex &index, const QRect &geometry, QObject *delegate)
{
    if (index.isValid() && index.model() == this) {
        d->abstractTasksSourceModel->requestPublishDelegateGeometry(mapToSource(index), geometry, delegate);
    }
}

void TasksModel::requestToggleGrouping(const QModelIndex &index)
{
    if (index.isValid() && index.model() == this) {
        const QModelIndex &target = (d->flattenGroupsProxyModel
            ? d->flattenGroupsProxyModel->mapToSource(mapToSource(index)) : mapToSource(index));
        d->groupingProxyModel->requestToggleGrouping(target);
    }
}

bool TasksModel::move(int row, int newPos)
{
    if (d->sortMode != SortManual || row == newPos || newPos < 0 || newPos >= rowCount()) {
        return false;
    }

    const QModelIndex &idx = index(row, 0);
    bool isLauncherMove = false;

    // Figure out if we're moving a launcher so we can run barrier checks.
    if (idx.isValid()) {
        if (idx.data(AbstractTasksModel::IsLauncher).toBool()) {
            isLauncherMove = true;
        // When using launch-in-place sorting, launcher-backed window tasks act as launchers.
        } else if ((d->launchInPlace || !d->separateLaunchers)
            && idx.data(AbstractTasksModel::IsWindow).toBool()) {
            const QUrl &launcherUrl = idx.data(AbstractTasksModel::LauncherUrl).toUrl();
            const int launcherPos = launcherPosition(launcherUrl);

            if (launcherPos != -1) {
                isLauncherMove = true;
            }
        }
    } else {
        return false;
    }

    if (d->separateLaunchers) {
        const int firstTask = (d->launcherTasksModel ?
            (d->launchInPlace ? d->launcherTasksModel->rowCount() : launcherCount()) : 0);

        // Don't allow launchers to be moved past the last launcher.
        if (isLauncherMove && newPos >= firstTask) {
            return false;
        }

        // Don't allow tasks to be moved into the launchers.
        if (!isLauncherMove && newPos < firstTask) {
            return false;
        }
    }

    // Treat flattened-out groups as single items.
    if (d->flattenGroupsProxyModel) {
        QModelIndex groupingRowIndex = d->flattenGroupsProxyModel->mapToSource(mapToSource(index(row, 0)));
        const QModelIndex &groupingRowIndexParent = groupingRowIndex.parent();
        QModelIndex groupingNewPosIndex = d->flattenGroupsProxyModel->mapToSource(mapToSource(index(newPos, 0)));
        const QModelIndex &groupingNewPosIndexParent = groupingNewPosIndex.parent();

        // Disallow moves within a flattened-out group (TODO: for now, anyway).
        if (groupingRowIndexParent.isValid()
            && (groupingRowIndexParent == groupingNewPosIndex
            || groupingRowIndexParent == groupingNewPosIndexParent)) {
            return false;
        }

        int offset = 0;
        int extraChildCount = 0;

        if (groupingRowIndexParent.isValid()) {
            offset = groupingRowIndex.row();
            extraChildCount = d->groupingProxyModel->rowCount(groupingRowIndexParent) - 1;
            groupingRowIndex = groupingRowIndexParent;
        }

        if (groupingNewPosIndexParent.isValid()) {
            int extra = d->groupingProxyModel->rowCount(groupingNewPosIndexParent) - 1;

            if (newPos > row) {
                newPos += extra;
                newPos -= groupingNewPosIndex.row();
                groupingNewPosIndex = groupingNewPosIndexParent.child(extra, 0);
            } else {
                newPos -= groupingNewPosIndex.row();
                groupingNewPosIndex = groupingNewPosIndexParent;
            }
        }

        beginMoveRows(QModelIndex(), (row - offset), (row - offset) + extraChildCount,
            QModelIndex(), (newPos > row) ? newPos + 1 : newPos);

        row = d->sortedPreFilterRows.indexOf(d->filterProxyModel->mapToSource(d->groupingProxyModel->mapToSource(groupingRowIndex)).row());
        newPos = d->sortedPreFilterRows.indexOf(d->filterProxyModel->mapToSource(d->groupingProxyModel->mapToSource(groupingNewPosIndex)).row());

        // Update sort mappings.
        d->sortedPreFilterRows.move(row, newPos);

        if (groupingRowIndexParent.isValid()) {
            d->consolidateManualSortMapForGroup(groupingRowIndexParent);
        }

        endMoveRows();
    } else {
        beginMoveRows(QModelIndex(), row, row, QModelIndex(), (newPos > row) ? newPos + 1 : newPos);

        // Translate to sort map indices.
        const QModelIndex &groupingRowIndex = mapToSource(index(row, 0));
        const QModelIndex &preFilterRowIndex = d->preFilterIndex(groupingRowIndex);
        row = d->sortedPreFilterRows.indexOf(preFilterRowIndex.row());
        newPos = d->sortedPreFilterRows.indexOf(d->preFilterIndex(mapToSource(index(newPos, 0))).row());

        // Update sort mapping.
        d->sortedPreFilterRows.move(row, newPos);

        // If we moved a group parent, consolidate sort map for children.
        if (groupMode() != GroupDisabled && d->groupingProxyModel->rowCount(groupingRowIndex)) {
            d->consolidateManualSortMapForGroup(groupingRowIndex);
        }

        endMoveRows();
    }

    // Resort.
    d->forceResort();

    if (!d->separateLaunchers && isLauncherMove) {
        const QModelIndex &idx = d->concatProxyModel->index(d->sortedPreFilterRows.at(newPos), 0);
        const QUrl &launcherUrl = idx.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl();

        // Move launcher for launcher-backed task along with task if launchers
        // are not being kept separate.
        // We don't need to resort again because the launcher is implictly hidden
        // at this time.
        if (!idx.data(AbstractTasksModel::IsLauncher).toBool()) {
            const int launcherPos = d->launcherTasksModel->launcherPosition(launcherUrl);
            const QModelIndex &launcherIndex = d->launcherTasksModel->index(launcherPos, 0);
            const int sortIndex = d->sortedPreFilterRows.indexOf(d->concatProxyModel->mapFromSource(launcherIndex).row());
            d->sortedPreFilterRows.move(sortIndex, newPos);
        // Otherwise move matching windows to after the launcher task (they are
        // currently hidden but might be on another virtual desktop).
        } else {
            for (int i = (d->sortedPreFilterRows.count() - 1); i >= 0; --i) {
                const QModelIndex &concatProxyIndex = d->concatProxyModel->index(d->sortedPreFilterRows.at(i), 0);

                if (launcherUrl == concatProxyIndex.data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl()) {
                    d->sortedPreFilterRows.move(i, newPos);

                    if (newPos > i) {
                        --newPos;
                    }
                }
            }
        }
    }

    // Setup for syncLaunchers().
    d->launcherSortingDirty = isLauncherMove;

    return true;
}

void TasksModel::syncLaunchers()
{
    // Writes the launcher order exposed through the model back to the launcher
    // tasks model, committing any move() operations to persistent state.

    if (!d->launcherTasksModel || !d->launcherSortingDirty) {
        return;
    }

    QMap<int, QString> sortedShownLaunchers;

    foreach(const QString &launcherUrlStr, launcherList()) {
        int row = -1;
        QStringList activities;
        QUrl launcherUrl;

        std::tie(launcherUrl, activities) = deserializeLauncher(launcherUrlStr);

        for (int i = 0; i < d->launcherTasksModel->rowCount(); ++i) {
            const QUrl &rowLauncherUrl =
                d->launcherTasksModel->index(i, 0).data(AbstractTasksModel::LauncherUrlWithoutIcon).toUrl();

            if (launcherUrlsMatch(launcherUrl, rowLauncherUrl, IgnoreQueryItems)) {
                row = i;
                break;
            }
        }

        if (row != -1) {
            sortedShownLaunchers.insert(row, launcherUrlStr);
        }
    }

    // Prep sort map for source model data changes.
    if (d->sortMode == SortManual) {
        QVector<int> sortMapIndices;
        QVector<int> preFilterRows;

        for (int i = 0; i < d->launcherTasksModel->rowCount(); ++i) {
            const QModelIndex &launcherIndex = d->launcherTasksModel->index(i, 0);
            const QModelIndex &concatIndex = d->concatProxyModel->mapFromSource(launcherIndex);
            sortMapIndices << d->sortedPreFilterRows.indexOf(concatIndex.row());
            preFilterRows << concatIndex.row();
        }

        // We're going to write back launcher model entries in the sort
        // map in concat model order, matching the reordered launcher list
        // we're about to pass down.
        std::sort(sortMapIndices.begin(), sortMapIndices.end());

        for (int i = 0; i < sortMapIndices.count(); ++i) {
            d->sortedPreFilterRows.replace(sortMapIndices.at(i), preFilterRows.at(i));
        }
    }

    setLauncherList(sortedShownLaunchers.values());
    d->launcherSortingDirty = false;
}

QModelIndex TasksModel::activeTask() const
{
    for (int i = 0; i < rowCount(); ++i) {
        const QModelIndex &idx = index(i, 0);

        if (idx.data(AbstractTasksModel::IsActive).toBool()) {
            if (groupMode() != GroupDisabled && rowCount(idx)) {
                for (int j = 0; j < rowCount(idx); ++j) {
                    const QModelIndex &child = idx.child(j, 0);

                    if (child.data(AbstractTasksModel::IsActive).toBool()) {
                        return child;
                    }
                }
            } else {
                return idx;
            }
        }
    }

    return QModelIndex();
}

QModelIndex TasksModel::makeModelIndex(int row, int childRow) const
{
    if (row < 0 || row >= rowCount()) {
        return QModelIndex();
    }

    if (childRow == -1) {
        return index(row, 0);
    } else {
        const QModelIndex &parent = index(row, 0);

        if (childRow < rowCount(parent)) {
            return parent.child(childRow, 0);
        }
    }

    return QModelIndex();
}

bool TasksModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    // All our filtering occurs at the top-level; anything below always
    // goes through.
    if (sourceParent.isValid()) {
        return true;
    }

    const QModelIndex &sourceIndex = sourceModel()->index(sourceRow, 0);

    // In inline grouping mode, filter out group parents.
    if (d->groupInline && d->flattenGroupsProxyModel
        && sourceIndex.data(AbstractTasksModel::IsGroupParent).toBool()) {
        return false;
    }

    const QString &appId = sourceIndex.data(AbstractTasksModel::AppId).toString();
    const QString &appName = sourceIndex.data(AbstractTasksModel::AppName).toString();

    // Filter startup tasks we already have a window task for.
    if (sourceIndex.data(AbstractTasksModel::IsStartup).toBool()) {
        for (int i = 0; i < d->filterProxyModel->rowCount(); ++i) {
            const QModelIndex &filterIndex = d->filterProxyModel->index(i, 0);

            if (!filterIndex.data(AbstractTasksModel::IsWindow).toBool()) {
                continue;
            }

            if ((!appId.isEmpty() && appId == filterIndex.data(AbstractTasksModel::AppId).toString())
                || (!appName.isEmpty() && appName == filterIndex.data(AbstractTasksModel::AppName).toString())) {
                return false;
            }
        }
    }

    // Filter launcher tasks we already have a startup or window task for (that
    // got through filtering).
    if (sourceIndex.data(AbstractTasksModel::IsLauncher).toBool()) {
        for (int i = 0; i < d->filterProxyModel->rowCount(); ++i) {
            const QModelIndex &filteredIndex = d->filterProxyModel->index(i, 0);

            if (!filteredIndex.data(AbstractTasksModel::IsWindow).toBool() &&
                !filteredIndex.data(AbstractTasksModel::IsStartup).toBool()) {
                continue;
            }

            if (appsMatch(sourceIndex, filteredIndex)) {
                return false;
            }
        }
    }

    return true;
}

bool TasksModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    // In manual sort mode, sort by map.
    if (d->sortMode == SortManual) {
        return (d->sortedPreFilterRows.indexOf(d->preFilterIndex(left).row())
            < d->sortedPreFilterRows.indexOf(d->preFilterIndex(right).row()));
    }

    return d->lessThan(left, right);
}

}

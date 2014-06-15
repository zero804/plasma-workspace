/*
 *   Copyright 2011 Viranch Mehta <viranch.mehta@gmail.com>
 *   Copyright 2013, 2014 Kai Uwe Broulik <kde@privat.broulik.de>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 or
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

import QtQuick 2.0
import org.kde.plasma.core 2.0 as PlasmaCore
import org.kde.plasma.components 2.0 as Components
import org.kde.plasma.extras 2.0 as PlasmaExtras
import org.kde.kquickcontrolsaddons 2.0
import "plasmapackage:/code/logic.js" as Logic

FocusScope {
    id: dialog
    focus: true

    property alias model: batteryList.model
    property bool pluggedIn

    property int remainingTime
    property bool showRemainingTime

    property bool isBrightnessAvailable
    property alias screenBrightness: brightnessSlider.value
    property alias screenBrightnessPercentage: brightnessSlider.percentage

    property bool isKeyboardBrightnessAvailable
    property alias keyboardBrightness: keyboardBrightnessSlider.value
    property alias keyboardBrightnessPercentage: keyboardBrightnessSlider.percentage

    signal brightnessChanged(int screenBrightness)
//    signal keyboardBrightnessChanged(int keyboardBrightness)
    signal powermanagementChanged(bool checked)

    PlasmaExtras.ScrollArea {
        id: batteryScrollArea
        anchors {
            left: parent.left
            right: parent.right
            top: plasmoid.location == PlasmaCore.Types.BottomEdge ? parent.top : settingsColumn.bottom
            bottom: plasmoid.location == PlasmaCore.Types.BottomEdge ? settingsColumn.top : parent.bottom
        }

        ListView {
            id: batteryList

            boundsBehavior: Flickable.StopAtBounds
            spacing: Math.round(units.gridUnit / 2)

            KeyNavigation.tab: brightnessSlider
            KeyNavigation.backtab: pmSwitch

            delegate: BatteryItem {}
        }
    }

    Column {
        id: settingsColumn
        spacing: 0
        width: parent.width

        anchors {
            left: parent.left
            right: parent.right
            top: plasmoid.location == PlasmaCore.Types.BottomEdge ? undefined : parent.top
            bottom: plasmoid.location == PlasmaCore.Types.BottomEdge ? parent.bottom : undefined
        }

        BrightnessItem {
            id: brightnessSlider
            icon: "DisplayBrightness"
            label: i18n("Display Brightness")
            visible: isBrightnessAvailable
            onChanged: brightnessChanged(value)
            KeyNavigation.tab: keyboardBrightnessSlider
            KeyNavigation.backtab: batteryList
            focus: true
        }

        BrightnessItem {
            id: keyboardBrightnessSlider
            icon: "KeyboardBrightness"
            label: i18n("Keyboard Brightness")
            visible: isKeyboardBrightnessAvailable
            onChanged: keyboardBrightnessChanged(value)
            KeyNavigation.tab: pmSwitch
            KeyNavigation.backtab: brightnessSlider
        }

        Components.Label {
            anchors {
                left: parent.left
                leftMargin: units.gridUnit * 1.5 + units.iconSizes.medium
                right: parent.right
                rightMargin: units.gridUnit
            }
            height: pmSwitch.height
            verticalAlignment: Text.AlignVCenter
            visible: !isKeyboardBrightnessAvailable && !isBrightnessAvailable
            text: i18n("No screen or keyboard brightness controls available")
            wrapMode: Text.Wrap
        }

        PowerManagementItem {
            id: pmSwitch
            onEnabledChanged: powermanagementChanged(enabled)
            KeyNavigation.tab: batteryList
            KeyNavigation.backtab: keyboardBrightnessSlider
        }
    }

}

import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

/**
 * Enterprise Application Sidebar Component
 * Pixel-exact parity with web console sidebar rail and active navigation pill
 */
Rectangle {
    id: root
    width: collapsed ? 64 : 240
    color: Theme.bgSurface
    border.color: Theme.borderSubtle
    border.width: 1

    property bool collapsed: false
    property string activeRoute: "live"

    signal routeSelected(string route)

    Behavior on width {
        NumberAnimation { duration: 180; easing.type: Easing.OutQuad }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Navigation Links
        ListView {
            id: navList
            Layout.fillWidth: true
            Layout.fillHeight: true
            interactive: false
            spacing: 4
            topMargin: 12
            leftMargin: 10
            rightMargin: 10

            model: [
                { route: "live", label: "Live Matrix", iconName: "live" },
                { route: "playback", label: "Recordings", iconName: "recordings" },
                { route: "cameras", label: "Camera Manager", iconName: "cameras" },
                { route: "dashboard", label: "Health & Telemetry", iconName: "dashboard" },
                { route: "system", label: "System & Storage", iconName: "system" }
            ]

            delegate: Rectangle {
                id: navItem
                required property var modelData
                width: navList.width - 20
                height: 44
                radius: 12
                color: {
                    if (root.activeRoute === navItem.modelData.route) return Theme.accentPrimary;
                    if (navMouse.containsMouse) return Theme.bgSecondary;
                    return "transparent";
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.collapsed ? 14 : 12
                    anchors.rightMargin: 12
                    spacing: 12

                    AppIcon {
                        name: navItem.modelData.iconName
                        size: 18
                        color: {
                            if (root.activeRoute === navItem.modelData.route) return "#ffffff";
                            if (navMouse.containsMouse) return Theme.textPrimary;
                            return Theme.textMuted;
                        }
                        strokeWidth: 2
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Text {
                        text: navItem.modelData.label
                        color: {
                            if (root.activeRoute === navItem.modelData.route) return "#ffffff";
                            if (navMouse.containsMouse) return Theme.textPrimary;
                            return Theme.textMuted;
                        }
                        font.pixelSize: 14
                        font.weight: root.activeRoute === navItem.modelData.route ? Font.DemiBold : Font.Medium
                        font.family: Theme.fontSans
                        visible: !root.collapsed
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                MouseArea {
                    id: navMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.activeRoute = navItem.modelData.route;
                        root.routeSelected(navItem.modelData.route);
                    }
                }
            }
        }

        // Bottom Rail Toggle
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: "transparent"

            Rectangle {
                anchors.fill: parent
                anchors.margins: 8
                radius: 12
                color: toggleMouse.containsMouse ? Theme.bgSecondary : "transparent"
                border.color: Theme.borderSubtle
                border.width: 1

                RowLayout {
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "collapse"
                        size: 14
                        color: toggleMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
                        strokeWidth: 2
                        rotation: root.collapsed ? 180 : 0
                    }

                    Text {
                        text: "Collapse rail"
                        color: toggleMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
                        font.pixelSize: 12
                        font.bold: true
                        font.family: Theme.fontSans
                        visible: !root.collapsed
                    }
                }

                MouseArea {
                    id: toggleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.collapsed = !root.collapsed
                }
            }
        }
    }
}

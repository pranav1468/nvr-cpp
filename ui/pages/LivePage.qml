import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

/**
 * Enterprise Flagship Live Surveillance Matrix & Split-Pane Console
 * Exact pixel replica of web console (web_live_actual.png)
 */
Item {
    id: root

    property string layoutMode: "4" // "1", "4", "7plus1", "8"
    property string prevLayoutMode: "4"
    property int selectedChannel: 1
    property bool isFullscreen: false
    property int fullscreenChannel: 1
    property bool sidebarCollapsed: false
    property var cameraList: []
    property string activeFilter: "all"
    property string searchQuery: ""

    function toggleFullscreen(ch) {
        if (root.isFullscreen || root.layoutMode === "1") {
            root.isFullscreen = false;
            root.layoutMode = root.prevLayoutMode || "4";
            var cnt = (root.layoutMode === "7plus1" || root.layoutMode === "8") ? 8 : (root.layoutMode === "1" ? 1 : 4);
            ApiClient.setLayout(cnt);
        } else {
            root.prevLayoutMode = root.layoutMode;
            root.fullscreenChannel = ch;
            root.selectedChannel = ch;
            root.isFullscreen = true;
            root.layoutMode = "1";
            ApiClient.setFullscreen(ch);
        }
    }

    function switchLayout(mode) {
        if (mode === "1") {
            if (root.layoutMode !== "1") {
                root.prevLayoutMode = root.layoutMode;
            }
            root.isFullscreen = true;
            root.layoutMode = "1";
            ApiClient.setLayout(1);
        } else {
            root.isFullscreen = false;
            root.layoutMode = mode;
            var cnt = (mode === "7plus1" || mode === "8") ? 8 : 4;
            ApiClient.setLayout(cnt);
        }
    }

    Component.onCompleted: {
        var cnt = (root.layoutMode === "7plus1" || root.layoutMode === "8") ? 8 : (root.layoutMode === "1" ? 1 : 4);
        ApiClient.setLayout(cnt);
        loadCameras();
    }

    function loadCameras() {
        ApiClient.getCameras(function(cams) {
            root.cameraList = cams || [];
        }, function() {
            root.cameraList = [];
        });
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        // =========================================================================
        // LEFT PANE: Camera Directory Card (w-72, rounded-2xl, border-border, shadow)
        // =========================================================================
        Rectangle {
            id: camPane
            Layout.fillHeight: true
            Layout.preferredWidth: root.sidebarCollapsed ? 0 : 280
            visible: !root.sidebarCollapsed
            color: Theme.bgCard
            border.color: Theme.borderSubtle
            border.width: 1
            radius: 16
            clip: true

            Behavior on Layout.preferredWidth {
                NumberAnimation { duration: 180; easing.type: Easing.OutQuad }
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Header
                Rectangle {
                    Layout.fillWidth: true
                    height: 44
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 16

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 8

                        AppIcon {
                            name: "video"
                            size: 16
                            color: Theme.accentPrimary
                            strokeWidth: 2
                        }

                        Text {
                            text: "CAMERA DIRECTORY"
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 0.5
                            font.family: Theme.fontDisplay
                        }

                        // Count Badge (0 / 8)
                        Rectangle {
                            color: Theme.bgSecondary
                            radius: 6
                            Layout.preferredHeight: 20
                            Layout.preferredWidth: countText.implicitWidth + 12

                            Text {
                                id: countText
                                anchors.centerIn: parent
                                text: root.cameraList.length + " / 8"
                                color: Theme.textSecondary
                                font.pixelSize: 11
                                font.bold: true
                                font.family: Theme.fontMono
                            }
                        }

                        Item { Layout.fillWidth: true }

                        // Collapse Button (<)
                        Rectangle {
                            width: 26
                            height: 26
                            radius: 6
                            color: collapseMouse.containsMouse ? Theme.bgSecondary : "transparent"

                            AppIcon {
                                anchors.centerIn: parent
                                name: "collapse"
                                size: 13
                                color: Theme.textMuted
                                strokeWidth: 2
                            }

                            MouseArea {
                                id: collapseMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.sidebarCollapsed = true
                            }
                        }
                    }
                }

                // Search Bar Input
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    height: 32
                    radius: 6
                    color: Theme.bgInput
                    border.color: searchFocus.activeFocus ? Theme.borderFocus : Theme.borderLight
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 6

                        AppIcon {
                            name: "search"
                            size: 13
                            color: Theme.textMuted
                            strokeWidth: 2
                        }

                        TextInput {
                            id: searchFocus
                            Layout.fillWidth: true
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                            verticalAlignment: TextInput.AlignVCenter
                            onTextChanged: root.searchQuery = text.trim().toLowerCase()

                            Text {
                                text: "Search cameras by name/IP..."
                                color: Theme.textDim
                                font.pixelSize: 12
                                font.family: Theme.fontSans
                                visible: !searchFocus.text && !searchFocus.activeFocus
                            }
                        }
                    }
                }

                // Quick Filter Pills (All, Online, Recording)
                RowLayout {
                    Layout.topMargin: 10
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    spacing: 6

                    Rectangle {
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: pillAllText.implicitWidth + 18
                        radius: 13
                        color: root.activeFilter === "all" ? Theme.accentPrimary : Theme.bgSecondary

                        Text {
                            id: pillAllText
                            anchors.centerIn: parent
                            text: "All"
                            color: root.activeFilter === "all" ? "#ffffff" : Theme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activeFilter = "all"
                        }
                    }

                    Rectangle {
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: pillOnlineText.implicitWidth + 18
                        radius: 13
                        color: root.activeFilter === "online" ? Theme.accentPrimary : Theme.bgSecondary

                        Text {
                            id: pillOnlineText
                            anchors.centerIn: parent
                            text: "Online"
                            color: root.activeFilter === "online" ? "#ffffff" : Theme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activeFilter = "online"
                        }
                    }

                    Rectangle {
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: pillRecText.implicitWidth + 18
                        radius: 13
                        color: root.activeFilter === "rec" ? Theme.accentPrimary : Theme.bgSecondary

                        Text {
                            id: pillRecText
                            anchors.centerIn: parent
                            text: "Recording"
                            color: root.activeFilter === "rec" ? "#ffffff" : Theme.textSecondary
                            font.pixelSize: 11
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activeFilter = "rec"
                        }
                    }
                }

                // Empty State or Camera List Tree
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: 10

                    // Empty State (Matches web console)
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 8
                        visible: root.cameraList.length === 0

                        Text {
                            text: "No cameras match."
                            color: Theme.textMuted
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            text: "+ Discover ONVIF"
                            color: Theme.accentPrimary
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                            Layout.alignment: Qt.AlignHCenter

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    // Navigate to Camera Manager
                                    var mainWin = root;
                                    while (mainWin.parent) mainWin = mainWin.parent;
                                    if (mainWin.currentRoute !== undefined) mainWin.currentRoute = "cameras";
                                }
                            }
                        }
                    }

                    // Populated Camera List
                    ListView {
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        spacing: 4
                        visible: root.cameraList.length > 0
                        model: root.cameraList

                        delegate: Rectangle {
                            width: parent.width
                            height: 44
                            radius: 8
                            color: (modelData.id === root.selectedChannel) ? Qt.rgba(0.07, 0.4, 0.83, 0.12) : (itemMouse.containsMouse ? Theme.bgSecondary : "transparent")
                            border.color: (modelData.id === root.selectedChannel) ? Theme.accentPrimary : "transparent"
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                Rectangle {
                                    width: 6
                                    height: 6
                                    radius: 3
                                    color: (modelData.status === "ONLINE" || modelData.status === "STREAMING") ? Theme.statusOnline : Theme.statusOffline
                                }

                                Text {
                                    text: modelData.name || ("Camera " + modelData.id)
                                    color: Theme.textPrimary
                                    font.pixelSize: 12
                                    font.bold: true
                                    font.family: Theme.fontSans
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "CH " + (modelData.id < 10 ? "0" + modelData.id : modelData.id)
                                    color: Theme.textMuted
                                    font.pixelSize: 10
                                    font.family: Theme.fontMono
                                }
                            }

                            MouseArea {
                                id: itemMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectedChannel = modelData.id
                            }
                        }
                    }
                }
            }
        }

        // =========================================================================
        // CENTER SURVEILLANCE STAGE: Toolbar & Video Matrix
        // =========================================================================
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Top Matrix Toolbar Card (rounded-2xl border-border bg-card shadow-panel)
            Rectangle {
                Layout.fillWidth: true
                height: 52
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 16

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 12

                    // Expand pane button (if collapsed)
                    Rectangle {
                        visible: root.sidebarCollapsed
                        width: 28
                        height: 28
                        radius: 6
                        color: expandMouse.containsMouse ? Theme.bgSecondary : "transparent"

                        AppIcon {
                            anchors.centerIn: parent
                            name: "collapse"
                            size: 14
                            color: Theme.textSecondary
                            strokeWidth: 2
                            rotation: 180
                        }

                        MouseArea {
                            id: expandMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.sidebarCollapsed = false
                        }
                    }

                    // Layout Switcher Container (bg-secondary rounded-xl p-1)
                    Rectangle {
                        color: Theme.bgSecondary
                        radius: 10
                        Layout.preferredHeight: 34
                        Layout.preferredWidth: layoutBtnRow.implicitWidth + 8

                        RowLayout {
                            id: layoutBtnRow
                            anchors.centerIn: parent
                            spacing: 4

                            // 1-CAM
                            Rectangle {
                                Layout.preferredHeight: 26
                                Layout.preferredWidth: btn1Text.implicitWidth + 18
                                radius: 6
                                color: root.layoutMode === "1" ? Theme.accentPrimary : "transparent"

                                Text {
                                    id: btn1Text
                                    anchors.centerIn: parent
                                    text: "1-CAM"
                                    color: root.layoutMode === "1" ? "#ffffff" : Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontSans
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.switchLayout("1")
                                }
                            }

                            // 4-CAM
                            Rectangle {
                                Layout.preferredHeight: 26
                                Layout.preferredWidth: btn4Text.implicitWidth + 18
                                radius: 6
                                color: root.layoutMode === "4" ? Theme.accentPrimary : "transparent"

                                Text {
                                    id: btn4Text
                                    anchors.centerIn: parent
                                    text: "4-CAM"
                                    color: root.layoutMode === "4" ? "#ffffff" : Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontSans
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.switchLayout("4")
                                }
                            }

                            // 7+1
                            Rectangle {
                                Layout.preferredHeight: 26
                                Layout.preferredWidth: btn7Text.implicitWidth + 18
                                radius: 6
                                color: root.layoutMode === "7plus1" ? Theme.accentPrimary : "transparent"

                                Text {
                                    id: btn7Text
                                    anchors.centerIn: parent
                                    text: "7+1"
                                    color: root.layoutMode === "7plus1" ? "#ffffff" : Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontSans
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.switchLayout("7plus1")
                                }
                            }

                            // 8-CAM
                            Rectangle {
                                Layout.preferredHeight: 26
                                Layout.preferredWidth: btn8Text.implicitWidth + 18
                                radius: 6
                                color: root.layoutMode === "8" ? Theme.accentPrimary : "transparent"

                                Text {
                                    id: btn8Text
                                    anchors.centerIn: parent
                                    text: "8-CAM"
                                    color: root.layoutMode === "8" ? "#ffffff" : Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontSans
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.switchLayout("8")
                                }
                            }
                        }
                    }


                    Item { Layout.fillWidth: true }

                    // Pipeline: Zero-Transcode StreamCopy Pill
                    Rectangle {
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: pipeText.implicitWidth + 20
                        radius: 8
                        color: Theme.bgBadgeBlue

                        Text {
                            id: pipeText
                            anchors.centerIn: parent
                            text: "Pipeline: Zero-Transcode StreamCopy"
                            color: Theme.accentPrimary
                            font.pixelSize: 11
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                    }
                }
            }

            // Video Matrix Surveillance Grid Area
            // Seamless multi-layout support: 1-CAM, 4-CAM, 7+1 Pro Master, and 8-CAM Full Matrix
            Item {
                id: matrixWrapper
                Layout.fillWidth: true
                Layout.fillHeight: true

                readonly property real gap: 8
                readonly property real w4: (width - gap) / 2
                readonly property real h4: (height - gap) / 2
                readonly property real w8: (width - 3 * gap) / 4
                readonly property real h8: (height - gap) / 2
                readonly property real w7: (width - 3 * gap) / 4
                readonly property real h7: (height - 3 * gap) / 4
                readonly property real masterW: 3 * w7 + 2 * gap
                readonly property real masterH: 3 * h7 + 2 * gap

                function getCameraName(chId) {
                    if (!root.cameraList) return "Empty";
                    for (var i = 0; i < root.cameraList.length; ++i) {
                        if (root.cameraList[i].id === chId) {
                            return root.cameraList[i].name || ("Camera " + chId);
                        }
                    }
                    return "Empty";
                }

                // CH 01 (Master in 7+1 mode, Fullscreen in 1-CAM)
                VideoTile {
                    id: tile1
                    tileIndex: 0
                    channelId: root.layoutMode === "1" ? root.fullscreenChannel : 1
                    cameraName: matrixWrapper.getCameraName(tile1.channelId)
                    visible: true
                    isSelected: root.selectedChannel === tile1.channelId
                    isFullscreen: root.layoutMode === "1"
                    streamType: (root.layoutMode === "1" || root.layoutMode === "7plus1") ? "MAIN" : "SUB"

                    x: 0
                    y: 0
                    width: root.layoutMode === "1" ? matrixWrapper.width :
                           (root.layoutMode === "4" ? matrixWrapper.w4 :
                           (root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.masterW))
                    height: root.layoutMode === "1" ? matrixWrapper.height :
                            (root.layoutMode === "4" ? matrixWrapper.h4 :
                            (root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.masterH))

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 02
                VideoTile {
                    id: tile2
                    tileIndex: 1
                    channelId: 2
                    cameraName: matrixWrapper.getCameraName(2)
                    visible: root.layoutMode !== "1"
                    isSelected: root.selectedChannel === 2
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "4" ? (matrixWrapper.w4 + matrixWrapper.gap) :
                       (root.layoutMode === "8" ? 1 * (matrixWrapper.w8 + matrixWrapper.gap) : 3 * (matrixWrapper.w7 + matrixWrapper.gap))
                    y: 0
                    width: root.layoutMode === "4" ? matrixWrapper.w4 :
                           (root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7)
                    height: root.layoutMode === "4" ? matrixWrapper.h4 :
                            (root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7)

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 03
                VideoTile {
                    id: tile3
                    tileIndex: 2
                    channelId: 3
                    cameraName: matrixWrapper.getCameraName(3)
                    visible: root.layoutMode !== "1"
                    isSelected: root.selectedChannel === 3
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "4" ? 0 :
                       (root.layoutMode === "8" ? 2 * (matrixWrapper.w8 + matrixWrapper.gap) : 3 * (matrixWrapper.w7 + matrixWrapper.gap))
                    y: root.layoutMode === "4" ? (matrixWrapper.h4 + matrixWrapper.gap) :
                       (root.layoutMode === "8" ? 0 : 1 * (matrixWrapper.h7 + matrixWrapper.gap))
                    width: root.layoutMode === "4" ? matrixWrapper.w4 :
                           (root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7)
                    height: root.layoutMode === "4" ? matrixWrapper.h4 :
                            (root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7)

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 04
                VideoTile {
                    id: tile4
                    tileIndex: 3
                    channelId: 4
                    cameraName: matrixWrapper.getCameraName(4)
                    visible: root.layoutMode !== "1"
                    isSelected: root.selectedChannel === 4
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "4" ? (matrixWrapper.w4 + matrixWrapper.gap) :
                       (root.layoutMode === "8" ? 3 * (matrixWrapper.w8 + matrixWrapper.gap) : 3 * (matrixWrapper.w7 + matrixWrapper.gap))
                    y: root.layoutMode === "4" ? (matrixWrapper.h4 + matrixWrapper.gap) :
                       (root.layoutMode === "8" ? 0 : 2 * (matrixWrapper.h7 + matrixWrapper.gap))
                    width: root.layoutMode === "4" ? matrixWrapper.w4 :
                           (root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7)
                    height: root.layoutMode === "4" ? matrixWrapper.h4 :
                            (root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7)

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 05
                VideoTile {
                    id: tile5
                    tileIndex: 4
                    channelId: 5
                    cameraName: matrixWrapper.getCameraName(5)
                    visible: root.layoutMode === "8" || root.layoutMode === "7plus1"
                    isSelected: root.selectedChannel === 5
                    isFullscreen: false
                    streamType: "SUB"

                    x: 0
                    y: root.layoutMode === "8" ? (matrixWrapper.h8 + matrixWrapper.gap) : 3 * (matrixWrapper.h7 + matrixWrapper.gap)
                    width: root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7
                    height: root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 06
                VideoTile {
                    id: tile6
                    tileIndex: 5
                    channelId: 6
                    cameraName: matrixWrapper.getCameraName(6)
                    visible: root.layoutMode === "8" || root.layoutMode === "7plus1"
                    isSelected: root.selectedChannel === 6
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "8" ? 1 * (matrixWrapper.w8 + matrixWrapper.gap) : 1 * (matrixWrapper.w7 + matrixWrapper.gap)
                    y: root.layoutMode === "8" ? (matrixWrapper.h8 + matrixWrapper.gap) : 3 * (matrixWrapper.h7 + matrixWrapper.gap)
                    width: root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7
                    height: root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 07
                VideoTile {
                    id: tile7
                    tileIndex: 6
                    channelId: 7
                    cameraName: matrixWrapper.getCameraName(7)
                    visible: root.layoutMode === "8" || root.layoutMode === "7plus1"
                    isSelected: root.selectedChannel === 7
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "8" ? 2 * (matrixWrapper.w8 + matrixWrapper.gap) : 2 * (matrixWrapper.w7 + matrixWrapper.gap)
                    y: root.layoutMode === "8" ? (matrixWrapper.h8 + matrixWrapper.gap) : 3 * (matrixWrapper.h7 + matrixWrapper.gap)
                    width: root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7
                    height: root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }

                // CH 08
                VideoTile {
                    id: tile8
                    tileIndex: 7
                    channelId: 8
                    cameraName: matrixWrapper.getCameraName(8)
                    visible: root.layoutMode === "8" || root.layoutMode === "7plus1"
                    isSelected: root.selectedChannel === 8
                    isFullscreen: false
                    streamType: "SUB"

                    x: root.layoutMode === "8" ? 3 * (matrixWrapper.w8 + matrixWrapper.gap) : 3 * (matrixWrapper.w7 + matrixWrapper.gap)
                    y: root.layoutMode === "8" ? (matrixWrapper.h8 + matrixWrapper.gap) : 3 * (matrixWrapper.h7 + matrixWrapper.gap)
                    width: root.layoutMode === "8" ? matrixWrapper.w8 : matrixWrapper.w7
                    height: root.layoutMode === "8" ? matrixWrapper.h8 : matrixWrapper.h7

                    onTileSelected: function(ch) { root.selectedChannel = ch; }
                    onFullscreenRequested: function(ch) { root.toggleFullscreen(ch); }
                }
            }
        }
    }
}

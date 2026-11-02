import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtMultimedia
import ".."
import "../components"

/**
 * Enterprise Recorded Footage Console & 24h Interactive Scrub Bar
 * Exact pixel & behavioral parity with the Web surveillance console
 */
Item {
    id: root

    // View mode: "cards" (Activity cards), "player" (Timeline player), "table" (Raw index)
    property string viewMode: "cards"

    // Data collections
    property var rawSegments: []
    property var reviewItems: []
    property var availableDates: []

    // Filter selections
    property string selectedChannel: ""
    property string selectedDate: ""
    property string selectedType: ""
    property string selectedRetention: ""
    property string statusMessage: ""

    // Player state
    property var activeItem: null
    property string activeVideoUrl: ""
    property double playbackRate: 1.0
    property bool isPlaying: false
    property bool autoNext: true
    property bool loopCurrent: false
    property double currentPositionSec: 0.0
    property double totalDurationSec: 60.0

    Component.onCompleted: {
        var now = new Date();
        var yyyy = now.getFullYear();
        var mm = String(now.getMonth() + 1).padStart(2, '0');
        var dd = String(now.getDate()).padStart(2, '0');
        root.selectedDate = yyyy + "-" + mm + "-" + dd;

        loadDates();
        searchFootage();
    }

    function loadDates() {
        ApiClient.queryRecordingDates(function(dates) {
            root.availableDates = dates || [];
            if (root.availableDates.length > 0 && (!root.selectedDate || root.selectedDate === "")) {
                var d0 = String(root.availableDates[0]);
                if (d0.length === 8) {
                    root.selectedDate = d0.substr(0, 4) + "-" + d0.substr(4, 2) + "-" + d0.substr(6, 2);
                } else {
                    root.selectedDate = d0;
                }
            }
        }, function(err) {
            console.warn("Failed to load recording dates: " + err);
        });
    }

    function searchFootage() {
        root.statusMessage = "Searching recorded footage index...";
        var filter = { limit: 200 };

        if (root.selectedChannel && root.selectedChannel !== "") {
            filter.channel_id = parseInt(root.selectedChannel, 10);
        }
        if (root.selectedDate && root.selectedDate !== "") {
            filter.date = root.selectedDate.replace(/-/g, "");
        }
        if (root.selectedType && root.selectedType !== "") {
            filter.trigger_type = root.selectedType;
        }

        ApiClient.queryRecordings(filter, function(segs, reviews) {
            root.rawSegments = segs || [];
            root.reviewItems = reviews || [];
            root.statusMessage = root.reviewItems.length + " activity events (" + root.rawSegments.length + " segments)";
        }, function(err) {
            root.statusMessage = "Query error: " + err;
        });
    }

    function playItem(item) {
        if (!item) return;
        root.activeItem = item;
        root.activeVideoUrl = ApiClient.getPlaybackUrl(item);
        root.totalDurationSec = item.duration_seconds ? item.duration_seconds : 60.0;
        root.currentPositionSec = 0.0;
        root.viewMode = "player";
        mediaPlayer.source = root.activeVideoUrl;
        mediaPlayer.play();
        root.isPlaying = true;
    }

    function toggleLock(item) {
        if (!item) return;
        var nextLock = !Boolean(item.is_locked);
        var targetId = item.id || (item.first_segment ? item.first_segment.id : "");
        var filePath = item.file_path || (item.first_segment ? item.first_segment.file_path : "");

        ApiClient.lockRecording(targetId, nextLock, filePath, function() {
            item.is_locked = nextLock;
            // Refresh bindings
            var copyR = root.reviewItems.slice(0);
            root.reviewItems = copyR;
            var copyS = root.rawSegments.slice(0);
            root.rawSegments = copyS;
        }, function(err) {
            root.statusMessage = "Failed to toggle lock: " + err;
        });
    }

    function openExportDialog(item) {
        clipExportModal.open(item || root.activeItem);
    }

    function formatDurationCompact(sec) {
        var s = Math.round(sec || 0);
        if (s < 60) return s + "s";
        var mins = Math.floor(s / 60);
        var rem = s % 60;
        if (mins < 60) return mins + "m " + (rem > 0 ? (rem < 10 ? "0" : "") + rem + "s" : "00s");
        var hrs = Math.floor(mins / 60);
        var remM = mins % 60;
        return hrs + "h " + (remM > 0 ? (remM < 10 ? "0" : "") + remM + "m" : "00m");
    }

    function secondsOfDayUtc(iso) {
        if (!iso) return 0;
        var d = new Date(iso);
        if (isNaN(d.getTime())) return 0;
        return d.getUTCHours() * 3600 + d.getUTCMinutes() * 60 + d.getUTCSeconds();
    }

    function formatTimeHMS(sec) {
        var s = Math.floor(sec || 0);
        var hrs = Math.floor(s / 3600);
        var mins = Math.floor((s % 3600) / 60);
        var rem = s % 60;
        var pad = function(n) { return (n < 10 ? "0" : "") + n; };
        return pad(hrs) + ":" + pad(mins) + ":" + pad(rem);
    }

    function formatFileSize(bytes) {
        if (!bytes || bytes <= 0) return "-- MB";
        var mb = bytes / (1024 * 1024);
        if (mb >= 1000) return (mb / 1024).toFixed(2) + " GB";
        return mb.toFixed(1) + " MB";
    }

    // Native Qt 6 Media Player
    MediaPlayer {
        id: mediaPlayer
        videoOutput: playerVideoOutput
        playbackRate: root.playbackRate
        loops: root.loopCurrent ? MediaPlayer.Infinite : MediaPlayer.Once

        onPositionChanged: {
            root.currentPositionSec = mediaPlayer.position / 1000.0;
        }

        onDurationChanged: {
            if (mediaPlayer.duration > 0) {
                root.totalDurationSec = mediaPlayer.duration / 1000.0;
            }
        }

        onPlaybackStateChanged: {
            root.isPlaying = (mediaPlayer.playbackState === MediaPlayer.PlayingState);
            if (mediaPlayer.playbackState === MediaPlayer.StoppedState && root.autoNext && !root.loopCurrent) {
                root.advanceToNextSequentialSegment();
            }
        }
    }

    function advanceToNextSequentialSegment() {
        if (!root.activeItem || root.rawSegments.length === 0) return;
        var curPath = root.activeItem.file_path || "";
        var idx = -1;
        for (var i = 0; i < root.rawSegments.length; ++i) {
            if (root.rawSegments[i].file_path === curPath) {
                idx = i;
                break;
            }
        }
        if (idx >= 0 && idx + 1 < root.rawSegments.length) {
            playItem(root.rawSegments[idx + 1]);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        // =========================================================================
        // 1. PAGE HEADER & 3-MODE VIEW SWITCHER (Exact Web Parity)
        // =========================================================================
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true

                Text {
                    text: "Footage review & recordings"
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.bold: true
                    font.family: Theme.fontDisplay
                }
                Text {
                    text: "Browse activity cards with hover preview, or scrub the precision 24-hour player"
                    color: Theme.textMuted
                    font.pixelSize: 13
                    font.family: Theme.fontSans
                }
            }

            // 3-Mode View Switcher Pills
            Rectangle {
                Layout.preferredHeight: 36
                Layout.preferredWidth: modeRow.implicitWidth + 8
                radius: 8
                color: Theme.bgSecondary
                border.color: Theme.borderSubtle
                border.width: 1

                RowLayout {
                    id: modeRow
                    anchors.centerIn: parent
                    spacing: 4

                    // Mode A: Activity cards
                    Rectangle {
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: cardBtnRow.implicitWidth + 16
                        radius: 6
                        color: root.viewMode === "cards" ? Theme.accentPrimary : "transparent"

                        RowLayout {
                            id: cardBtnRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "cards"
                                size: 14
                                color: root.viewMode === "cards" ? "#ffffff" : Theme.textMuted
                                strokeWidth: 2
                            }

                            Text {
                                text: "Activity cards"
                                color: root.viewMode === "cards" ? "#ffffff" : Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: root.viewMode === "cards" ? Font.DemiBold : Font.Medium
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.viewMode = "cards"
                        }
                    }

                    // Mode B: Timeline player
                    Rectangle {
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: playerBtnRow.implicitWidth + 16
                        radius: 6
                        color: root.viewMode === "player" ? Theme.accentPrimary : "transparent"

                        RowLayout {
                            id: playerBtnRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "play"
                                size: 13
                                color: root.viewMode === "player" ? "#ffffff" : Theme.textMuted
                                strokeWidth: 2
                            }

                            Text {
                                text: "Timeline player"
                                color: root.viewMode === "player" ? "#ffffff" : Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: root.viewMode === "player" ? Font.DemiBold : Font.Medium
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.viewMode = "player"
                        }
                    }

                    // Mode C: Raw index
                    Rectangle {
                        Layout.preferredHeight: 28
                        Layout.preferredWidth: tableBtnRow.implicitWidth + 16
                        radius: 6
                        color: root.viewMode === "table" ? Theme.accentPrimary : "transparent"

                        RowLayout {
                            id: tableBtnRow
                            anchors.centerIn: parent
                            spacing: 6

                            AppIcon {
                                name: "list"
                                size: 14
                                color: root.viewMode === "table" ? "#ffffff" : Theme.textMuted
                                strokeWidth: 2
                            }

                            Text {
                                text: "Raw index"
                                color: root.viewMode === "table" ? "#ffffff" : Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: root.viewMode === "table" ? Font.DemiBold : Font.Medium
                                font.family: Theme.fontSans
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.viewMode = "table"
                        }
                    }
                }
            }
        }

        // =========================================================================
        // 2. FILTER TOOLBAR CARD (Stacked Labels, Exact Web Parity)
        // =========================================================================
        Rectangle {
            Layout.fillWidth: true
            height: 72
            color: Theme.bgCard
            border.color: Theme.borderSubtle
            border.width: 1
            radius: 14

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 14

                // Channel Selector
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: "Channel"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }
                    ComboBox {
                        id: channelCombo
                        Layout.preferredWidth: 150
                        Layout.preferredHeight: 36
                        currentIndex: 0
                        model: ["All channels", "Channel 1", "Channel 2", "Channel 3", "Channel 4", "Channel 5", "Channel 6", "Channel 7", "Channel 8"]
                        displayText: (model && model.length > currentIndex && currentIndex >= 0) ? model[currentIndex] : "All channels"
                        background: Rectangle {
                            color: Theme.bgCard
                            border.color: Theme.borderSubtle
                            border.width: 1
                            radius: 6
                        }
                        contentItem: Text {
                            text: channelCombo.displayText
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 10
                            rightPadding: 24
                            elide: Text.ElideRight
                        }
                        indicator: AppIcon {
                            x: channelCombo.width - width - 10
                            y: (channelCombo.height - height) / 2
                            name: "chevron_down"
                            size: 11
                            color: Theme.textMuted
                        }
                        onCurrentIndexChanged: {
                            if (currentIndex === 0) root.selectedChannel = "";
                            else root.selectedChannel = String(currentIndex);
                        }
                    }
                }

                // Date Picker
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: "Date"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }
                    Rectangle {
                        Layout.preferredWidth: 150
                        Layout.preferredHeight: 36
                        color: Theme.bgCard
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: 6

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 6

                            TextInput {
                                id: dateInput
                                Layout.fillWidth: true
                                text: root.selectedDate
                                color: Theme.textPrimary
                                font.family: Theme.fontMono
                                font.pixelSize: 12
                                verticalAlignment: TextInput.AlignVCenter
                                onTextChanged: root.selectedDate = text
                            }

                            AppIcon {
                                name: "calendar"
                                size: 14
                                color: Theme.textMuted
                                strokeWidth: 1.75
                            }
                        }
                    }
                }

                // From Time Picker
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: "From"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }
                    Rectangle {
                        Layout.preferredWidth: 105
                        Layout.preferredHeight: 36
                        color: Theme.bgCard
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: 6

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 4

                            TextInput {
                                id: fromInput
                                Layout.fillWidth: true
                                text: "00:00"
                                color: Theme.textPrimary
                                font.family: Theme.fontMono
                                font.pixelSize: 12
                                verticalAlignment: TextInput.AlignVCenter
                            }

                            AppIcon {
                                name: "clock"
                                size: 13
                                color: Theme.textMuted
                                strokeWidth: 1.75
                            }
                        }
                    }
                }

                // To Time Picker
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: "To"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }
                    Rectangle {
                        Layout.preferredWidth: 105
                        Layout.preferredHeight: 36
                        color: Theme.bgCard
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: 6

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 4

                            TextInput {
                                id: toInput
                                Layout.fillWidth: true
                                text: "23:59"
                                color: Theme.textPrimary
                                font.family: Theme.fontMono
                                font.pixelSize: 12
                                verticalAlignment: TextInput.AlignVCenter
                            }

                            AppIcon {
                                name: "clock"
                                size: 13
                                color: Theme.textMuted
                                strokeWidth: 1.75
                            }
                        }
                    }
                }

                // Event Type Selector
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: "Activity type"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }
                    ComboBox {
                        id: typeCombo
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 36
                        currentIndex: 0
                        model: ["All activity", "AI alerts & events", "Motion activity", "24x7 Continuous"]
                        displayText: (model && model.length > currentIndex && currentIndex >= 0) ? model[currentIndex] : "All activity"
                        background: Rectangle {
                            color: Theme.bgCard
                            border.color: Theme.borderSubtle
                            border.width: 1
                            radius: 6
                        }
                        contentItem: Text {
                            text: typeCombo.displayText
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 10
                            rightPadding: 24
                            elide: Text.ElideRight
                        }
                        indicator: AppIcon {
                            x: typeCombo.width - width - 10
                            y: (typeCombo.height - height) / 2
                            name: "chevron_down"
                            size: 11
                            color: Theme.textMuted
                        }
                        onCurrentIndexChanged: {
                            if (currentIndex === 0) root.selectedType = "";
                            else if (currentIndex === 1) root.selectedType = "ai_event";
                            else if (currentIndex === 2) root.selectedType = "motion";
                            else if (currentIndex === 3) root.selectedType = "continuous";
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Search Footage Button (Filter activity)
                Rectangle {
                    Layout.alignment: Qt.AlignBottom
                    Layout.preferredHeight: 36
                    Layout.preferredWidth: searchBtnRow.implicitWidth + 28
                    radius: 6
                    color: Theme.accentPrimary

                    RowLayout {
                        id: searchBtnRow
                        anchors.centerIn: parent
                        spacing: 8

                        AppIcon {
                            name: "search"
                            size: 14
                            color: "#ffffff"
                            strokeWidth: 2
                        }

                        Text {
                            text: "Filter activity"
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.searchFootage()
                    }
                }
            }
        }

        // Summary Bar (Count & Info)
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            spacing: 12

            Text {
                text: (root.reviewItems.length + root.rawSegments.length) + " activity events found"
                color: Theme.textMuted
                font.pixelSize: 12
                font.weight: Font.Medium
                font.family: Theme.fontSans
            }

            Item { Layout.fillWidth: true }

            RowLayout {
                spacing: 6
                AppIcon {
                    name: "info"
                    size: 13
                    color: Theme.textMuted
                    strokeWidth: 1.75
                }
                Text {
                    text: "Hover cards to preview & scrub · Click card to open full player"
                    color: Theme.textMuted
                    font.pixelSize: 11
                    font.family: Theme.fontSans
                }
            }
        }

        // =========================================================================
        // 3. MAIN CONTENT: 3-VIEW SWITCHER AREA
        // =========================================================================
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ---------------------------------------------------------------------
            // SUB-VIEW A: FRIGATE-STYLE ACTIVITY CARDS
            // ---------------------------------------------------------------------
            Item {
                id: cardsViewContainer
                anchors.fill: parent
                visible: root.viewMode === "cards"

                // Dashed-Border Empty State Card (Exact Parity with Web)
                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 180
                    radius: 16
                    color: Theme.bgCard
                    border.color: Theme.borderLight
                    border.width: 1
                    visible: (root.reviewItems.length === 0 && root.rawSegments.length === 0)

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 8

                        AppIcon {
                            name: "video"
                            size: 38
                            color: Theme.textDim
                            strokeWidth: 1.75
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            text: "No recorded activity discovered"
                            color: Theme.textPrimary
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontSans
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            text: "No footage matched the selected camera channel, date, or event criteria."
                            color: Theme.textMuted
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }

                ScrollView {
                    id: cardsScrollView
                    anchors.fill: parent
                    visible: (root.reviewItems.length > 0 || root.rawSegments.length > 0)
                    clip: true

                    GridView {
                        id: cardsGrid
                        width: cardsScrollView.width
                        cellWidth: Math.max(300, Math.floor(cardsGrid.width / 4))
                        cellHeight: Math.floor(cellWidth * 0.78)
                        model: root.reviewItems.length > 0 ? root.reviewItems : root.rawSegments

                    delegate: Item {
                        id: cardDelegate
                        required property var modelData
                        width: cardsGrid.cellWidth - 12
                        height: cardsGrid.cellHeight - 12

                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusLg
                            color: Theme.bgSurface
                            border.color: Theme.borderSubtle
                            border.width: 1
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                // 16:9 Thumbnail Viewport Stage
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: Theme.bgTileStage
                                    clip: true

                                    Image {
                                        id: cardThumb
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectCrop
                                        source: ApiClient.getThumbnailUrl(cardDelegate.modelData)
                                        asynchronous: true
                                        cache: true
                                    }

                                    // Channel Pill (Top Left)
                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.margins: 8
                                        height: 20
                                        width: chText.implicitWidth + 12
                                        radius: Theme.radiusSm
                                        color: Qt.rgba(0, 0, 0, 0.65)

                                        Text {
                                            id: chText
                                            anchors.centerIn: parent
                                            text: "CH " + (cardDelegate.modelData.channel_id || 1)
                                            color: "#ffffff"
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: Theme.fontMono
                                        }
                                    }

                                    // Event Type Pill (Top Right)
                                    Rectangle {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 8
                                        height: 20
                                        width: typeText.implicitWidth + 12
                                        radius: Theme.radiusSm
                                        color: {
                                            var t = cardDelegate.modelData.trigger_type || "continuous";
                                            if (t === "motion") return Theme.statusWarning;
                                            if (t === "ai_event") return Theme.statusOnline;
                                            return Theme.accentPrimary;
                                        }

                                        Text {
                                            id: typeText
                                            anchors.centerIn: parent
                                            text: {
                                                var t = cardDelegate.modelData.trigger_type || "continuous";
                                                if (t === "motion") return "Motion";
                                                if (t === "ai_event") return "AI Event";
                                                return "Continuous";
                                            }
                                            color: "#ffffff"
                                            font.pixelSize: 9
                                            font.bold: true
                                            font.family: Theme.fontSans
                                        }
                                    }

                                    // Duration Badge (Bottom Right)
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.right: parent.right
                                        anchors.margins: 8
                                        height: 20
                                        width: durText.implicitWidth + 12
                                        radius: Theme.radiusSm
                                        color: Qt.rgba(0, 0, 0, 0.8)

                                        Text {
                                            id: durText
                                            anchors.centerIn: parent
                                            text: root.formatDurationCompact(cardDelegate.modelData.duration_seconds || 60)
                                            color: "#ffffff"
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: Theme.fontMono
                                        }
                                    }

                                    // Hover Play Icon Overlay
                                    Rectangle {
                                        anchors.fill: parent
                                        color: Qt.rgba(0, 0, 0, 0.3)
                                        visible: cardMouse.containsMouse

                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 42
                                            height: 42
                                            radius: 21
                                            color: Theme.accentPrimary

                                            Text {
                                                anchors.centerIn: parent
                                                text: "▶"
                                                color: Theme.accentPrimaryFg
                                                font.pixelSize: 16
                                                leftPadding: 2
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: cardMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.playItem(cardDelegate.modelData)
                                    }
                                }

                                // Card Bottom Info Row
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 46
                                    color: Theme.bgSurface
                                    border.color: Theme.borderSubtle
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 6

                                        ColumnLayout {
                                            spacing: 1
                                            Layout.fillWidth: true

                                            Text {
                                                text: (cardDelegate.modelData.start_time || "00:00:00") + " ~ " + (cardDelegate.modelData.end_time || "")
                                                color: Theme.textPrimary
                                                font.pixelSize: 11
                                                font.bold: true
                                                font.family: Theme.fontMono
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                text: (cardDelegate.modelData.segment_count ? (cardDelegate.modelData.segment_count + " segs · ") : "") + root.formatFileSize(cardDelegate.modelData.file_size_bytes)
                                                color: Theme.textMuted
                                                font.pixelSize: 10
                                                font.family: Theme.fontSans
                                            }
                                        }

                                        // Lock Button
                                        Rectangle {
                                            Layout.preferredWidth: 26
                                            Layout.preferredHeight: 26
                                            radius: Theme.radiusSm
                                            color: cardDelegate.modelData.is_locked ? Qt.rgba(0.86, 0.15, 0.15, 0.15) : Theme.bgSecondary
                                            border.color: cardDelegate.modelData.is_locked ? Theme.statusRec : Theme.borderSubtle
                                            border.width: 1

                                            Text {
                                                anchors.centerIn: parent
                                                text: cardDelegate.modelData.is_locked ? "🔒" : "🔓"
                                                font.pixelSize: 11
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.toggleLock(cardDelegate.modelData)
                                            }
                                        }

                                        // Export Button
                                        Rectangle {
                                            Layout.preferredWidth: 26
                                            Layout.preferredHeight: 26
                                            radius: Theme.radiusSm
                                            color: Theme.bgSecondary
                                            border.color: Theme.borderSubtle
                                            border.width: 1

                                            Text {
                                                anchors.centerIn: parent
                                                text: "⤓"
                                                color: Theme.textSecondary
                                                font.pixelSize: 12
                                                font.bold: true
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.openExportDialog(cardDelegate.modelData)
                                            }
                                        }

                                        // Play Action Button
                                        Rectangle {
                                            Layout.preferredWidth: 48
                                            Layout.preferredHeight: 26
                                            radius: Theme.radiusSm
                                            color: Theme.accentPrimary

                                            Text {
                                                anchors.centerIn: parent
                                                text: "Play"
                                                color: Theme.accentPrimaryFg
                                                font.pixelSize: 10
                                                font.bold: true
                                                font.family: Theme.fontSans
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.playItem(cardDelegate.modelData)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

            // ---------------------------------------------------------------------
            // SUB-VIEW B: 24-HOUR TIMELINE PLAYER
            // ---------------------------------------------------------------------
            Rectangle {
                anchors.fill: parent
                visible: root.viewMode === "player"
                radius: Theme.radiusLg
                color: Theme.bgSurface
                border.color: Theme.borderSubtle
                border.width: 1
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12

                    // 16:9 Dedicated Video Surface
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.bgTileStage
                        radius: Theme.radiusMd
                        clip: true

                        VideoOutput {
                            id: playerVideoOutput
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectFit
                        }

                        // Top-Left HUD Info Overlay
                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.margins: 12
                            height: 26
                            width: hudRow.implicitWidth + 16
                            radius: Theme.radiusSm
                            color: Qt.rgba(0, 0, 0, 0.7)

                            RowLayout {
                                id: hudRow
                                anchors.centerIn: parent
                                spacing: 8

                                Text {
                                    text: "CH " + (root.activeItem ? (root.activeItem.channel_id || 1) : 1)
                                    color: "#ffffff"
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontMono
                                }
                                Text {
                                    text: "·"
                                    color: "#94a3b8"
                                    font.bold: true
                                }
                                Text {
                                    text: "Zero-Transcode Direct StreamCopy"
                                    color: "#38bdf8"
                                    font.pixelSize: 10
                                    font.family: Theme.fontSans
                                }
                            }
                        }

                        // Top-Right Status Badge
                        Rectangle {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 12
                            height: 24
                            width: statusBadgeText.implicitWidth + 14
                            radius: Theme.radiusSm
                            color: root.isPlaying ? Theme.statusOnline : Theme.bgSecondary

                            Text {
                                id: statusBadgeText
                                anchors.centerIn: parent
                                text: root.isPlaying ? "PLAYING" : "PAUSED"
                                color: root.isPlaying ? "#ffffff" : Theme.textSecondary
                                font.pixelSize: 10
                                font.bold: true
                                font.family: Theme.fontSans
                            }
                        }

                        // Placeholder when no video loaded
                        ColumnLayout {
                            anchors.centerIn: parent
                            visible: !root.activeItem
                            spacing: 8

                            Text {
                                text: "Select an activity card or segment to begin playback"
                                color: "#94a3b8"
                                font.pixelSize: 13
                                font.family: Theme.fontSans
                            }
                        }
                    }

                    // 24-Hour Interactive Timeline Scrubber
                    Rectangle {
                        Layout.fillWidth: true
                        height: 58
                        color: Theme.bgSecondary
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: Theme.radiusMd

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 2

                            // 24h Ruler Hours
                            RowLayout {
                                Layout.fillWidth: true
                                Repeater {
                                    model: ["00:00", "03:00", "06:00", "09:00", "12:00", "15:00", "18:00", "21:00", "24:00"]
                                    Text {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        text: modelData
                                        color: Theme.textMuted
                                        font.pixelSize: 9
                                        font.family: Theme.fontMono
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }
                            }

                            // Interactive 24-Hour Timeline Bar
                            Rectangle {
                                id: timelineTrack
                                Layout.fillWidth: true
                                height: 26
                                color: Theme.bgCard
                                border.color: Theme.borderSubtle
                                border.width: 1
                                radius: 4
                                clip: true

                                // Multi-segment timeline blocks (Continuous = blue, Motion = amber, Event = red)
                                Repeater {
                                    model: root.rawSegments
                                    delegate: Rectangle {
                                        required property var modelData
                                        property real startSod: root.secondsOfDayUtc(modelData.start_time)
                                        property real segDur: modelData.duration_seconds || 60
                                        x: Math.max(0, (startSod / 86400.0) * timelineTrack.width)
                                        width: Math.max(4, (segDur / 86400.0) * timelineTrack.width)
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        anchors.topMargin: 2
                                        anchors.bottomMargin: 2
                                        radius: 2
                                        color: modelData.trigger_type === "motion" ? Theme.statusWarning :
                                               (modelData.trigger_type === "event" ? Theme.statusDanger : Theme.accentPrimary)
                                        opacity: (root.activeItem && (root.activeItem.id === modelData.id || root.activeItem.file_path === modelData.file_path)) ? 1.0 : 0.75

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.playItem(modelData);
                                            }
                                        }
                                    }
                                }

                                // 24-Hour Playhead Red Needle
                                Rectangle {
                                    id: playheadNeedle
                                    property real activeStartSod: root.activeItem ? root.secondsOfDayUtc(root.activeItem.start_time) : 0
                                    property real currentSod: activeStartSod + root.currentPositionSec
                                    x: root.activeItem ? Math.max(0, Math.min(timelineTrack.width - 2, (currentSod / 86400.0) * timelineTrack.width)) : 0
                                    visible: root.activeItem !== null
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: Theme.statusRec
                                    z: 10
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: function(mouse) {
                                        if (timelineTrack.width <= 0) return;
                                        var ratio = Math.max(0.0, Math.min(1.0, mouse.x / timelineTrack.width));
                                        var clickedSod = ratio * 86400.0;

                                        var hit = null;
                                        var bestDist = 9999999;
                                        var nearest = null;

                                        for (var i = 0; i < root.rawSegments.length; ++i) {
                                            var seg = root.rawSegments[i];
                                            var s = root.secondsOfDayUtc(seg.start_time);
                                            var e = s + (seg.duration_seconds || 60);
                                            if (clickedSod >= s && clickedSod <= e) {
                                                hit = seg;
                                                break;
                                            }
                                            var dist = Math.abs(clickedSod - s);
                                            if (dist < bestDist) {
                                                bestDist = dist;
                                                nearest = seg;
                                            }
                                        }

                                        var targetSeg = hit || nearest;
                                        if (targetSeg) {
                                            var segStart = root.secondsOfDayUtc(targetSeg.start_time);
                                            var offsetSec = hit ? Math.max(0.0, clickedSod - segStart) : 0.0;
                                            if (!root.activeItem || root.activeItem.id !== targetSeg.id) {
                                                root.playItem(targetSeg);
                                                if (offsetSec > 0) {
                                                    mediaPlayer.position = offsetSec * 1000.0;
                                                }
                                            } else {
                                                mediaPlayer.position = offsetSec * 1000.0;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Transport Control Bar
                    Rectangle {
                        Layout.fillWidth: true
                        height: 48
                        color: Theme.bgSurface
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: Theme.radiusMd

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            // Step -1s
                            Rectangle {
                                Layout.preferredWidth: 38
                                Layout.preferredHeight: 30
                                radius: Theme.radiusSm
                                color: Theme.bgSecondary

                                Text {
                                    anchors.centerIn: parent
                                    text: "-1s"
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontMono
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: mediaPlayer.position = Math.max(0, mediaPlayer.position - 1000)
                                }
                            }

                            // Play / Pause Button
                            Rectangle {
                                Layout.preferredHeight: 32
                                Layout.preferredWidth: playBtnRow.implicitWidth + 24
                                radius: Theme.radiusMd
                                color: Theme.accentPrimary

                                RowLayout {
                                    id: playBtnRow
                                    anchors.centerIn: parent
                                    spacing: 6

                                    Text {
                                        text: root.isPlaying ? "❚❚" : "▶"
                                        color: Theme.accentPrimaryFg
                                        font.pixelSize: 12
                                    }
                                    Text {
                                        text: root.isPlaying ? "Pause" : "Play"
                                        color: Theme.accentPrimaryFg
                                        font.pixelSize: 11
                                        font.bold: true
                                        font.family: Theme.fontSans
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (root.isPlaying) {
                                            mediaPlayer.pause();
                                        } else {
                                            mediaPlayer.play();
                                        }
                                    }
                                }
                            }

                            // Step +1s
                            Rectangle {
                                Layout.preferredWidth: 38
                                Layout.preferredHeight: 30
                                radius: Theme.radiusSm
                                color: Theme.bgSecondary

                                Text {
                                    anchors.centerIn: parent
                                    text: "+1s"
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontMono
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: mediaPlayer.position = Math.min(mediaPlayer.duration, mediaPlayer.position + 1000)
                                }
                            }

                            // Speed Pills
                            RowLayout {
                                spacing: 2
                                Repeater {
                                    model: [0.5, 1.0, 2.0, 4.0, 8.0]
                                    Rectangle {
                                        id: rateBtn
                                        required property var modelData
                                        Layout.preferredHeight: 28
                                        Layout.preferredWidth: 36
                                        radius: Theme.radiusSm
                                        color: root.playbackRate === rateBtn.modelData ? Theme.accentPrimary : Theme.bgSecondary

                                        Text {
                                            anchors.centerIn: parent
                                            text: rateBtn.modelData + "x"
                                            color: root.playbackRate === rateBtn.modelData ? Theme.accentPrimaryFg : Theme.textSecondary
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: Theme.fontMono
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.playbackRate = rateBtn.modelData;
                                                mediaPlayer.playbackRate = rateBtn.modelData;
                                            }
                                        }
                                    }
                                }
                            }

                            // Auto-Next Toggle
                            Rectangle {
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: autoNextRow.implicitWidth + 14
                                radius: Theme.radiusSm
                                color: root.autoNext ? Theme.bgAccent : Theme.bgSecondary
                                border.color: root.autoNext ? Theme.accentPrimary : Theme.borderSubtle
                                border.width: 1

                                RowLayout {
                                    id: autoNextRow
                                    anchors.centerIn: parent
                                    spacing: 4

                                    Rectangle {
                                        width: 6
                                        height: 6
                                        radius: 3
                                        color: root.autoNext ? Theme.statusOnline : Theme.textMuted
                                    }
                                    Text {
                                        text: "Auto-Next"
                                        color: root.autoNext ? Theme.textAccent : Theme.textSecondary
                                        font.pixelSize: 10
                                        font.family: Theme.fontSans
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.autoNext = !root.autoNext
                                }
                            }

                            // Loop Toggle
                            Rectangle {
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: 46
                                radius: Theme.radiusSm
                                color: root.loopCurrent ? Theme.bgAccent : Theme.bgSecondary
                                border.color: root.loopCurrent ? Theme.accentPrimary : Theme.borderSubtle
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: "Loop"
                                    color: root.loopCurrent ? Theme.textAccent : Theme.textSecondary
                                    font.pixelSize: 10
                                    font.family: Theme.fontSans
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.loopCurrent = !root.loopCurrent
                                }
                            }

                            // Export Clip Button
                            Rectangle {
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: exportBtnRow.implicitWidth + 16
                                radius: Theme.radiusSm
                                color: Theme.bgSecondary
                                border.color: Theme.accentPrimary
                                border.width: 1

                                RowLayout {
                                    id: exportBtnRow
                                    anchors.centerIn: parent
                                    spacing: 4

                                    Text {
                                        text: "⤓"
                                        color: Theme.textAccent
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    Text {
                                        text: "Export Clip"
                                        color: Theme.textAccent
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.family: Theme.fontSans
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openExportDialog(root.activeItem)
                                }
                            }

                            Item { Layout.fillWidth: true }

                            // Time Counter
                            Text {
                                text: root.formatTimeHMS(root.currentPositionSec) + " / " + root.formatTimeHMS(root.totalDurationSec)
                                color: Theme.textPrimary
                                font.pixelSize: 12
                                font.bold: true
                                font.family: Theme.fontMono
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------------------------
            // SUB-VIEW C: RAW SEGMENTS INDEX TABLE
            // ---------------------------------------------------------------------
            Rectangle {
                anchors.fill: parent
                visible: root.viewMode === "table"
                radius: Theme.radiusLg
                color: Theme.bgSurface
                border.color: Theme.borderSubtle
                border.width: 1
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    // Table Header
                    Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        color: Theme.bgSecondary
                        radius: Theme.radiusMd

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Text { Layout.preferredWidth: 160; text: "Segment ID"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 70; text: "Channel"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 140; text: "Start (UTC)"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 140; text: "End"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 80; text: "Duration"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 80; text: "Size"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Text { Layout.preferredWidth: 90; text: "Retention"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans }
                            Item { Layout.fillWidth: true }
                            Text { Layout.preferredWidth: 60; text: "Action"; color: Theme.textMuted; font.pixelSize: 11; font.bold: true; font.family: Theme.fontSans; horizontalAlignment: Text.AlignRight }
                        }
                    }

                    // Table Rows
                    ListView {
                        id: tableList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: root.rawSegments

                        delegate: Rectangle {
                            id: rowDelegate
                            required property var modelData
                            width: tableList.width
                            height: 38
                            radius: 4
                            color: rowMouse.containsMouse ? Theme.bgSecondary : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    Layout.preferredWidth: 160
                                    text: rowDelegate.modelData.id || "seg"
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    font.family: Theme.fontMono
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.preferredWidth: 70
                                    text: "CH " + (rowDelegate.modelData.channel_id || 1)
                                    color: Theme.accentPrimary
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontMono
                                }

                                Text {
                                    Layout.preferredWidth: 140
                                    text: rowDelegate.modelData.start_time || "--:--:--"
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.family: Theme.fontMono
                                }

                                Text {
                                    Layout.preferredWidth: 140
                                    text: rowDelegate.modelData.end_time || "--:--:--"
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.family: Theme.fontMono
                                }

                                Text {
                                    Layout.preferredWidth: 80
                                    text: (rowDelegate.modelData.duration_seconds || 60) + "s"
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    font.family: Theme.fontMono
                                }

                                Text {
                                    Layout.preferredWidth: 80
                                    text: root.formatFileSize(rowDelegate.modelData.file_size_bytes)
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                    font.family: Theme.fontMono
                                }

                                // Retention Lock Toggle
                                Rectangle {
                                    Layout.preferredWidth: 74
                                    Layout.preferredHeight: 24
                                    radius: Theme.radiusSm
                                    color: rowDelegate.modelData.is_locked ? Qt.rgba(0.86, 0.15, 0.15, 0.15) : Theme.bgSecondary
                                    border.color: rowDelegate.modelData.is_locked ? Theme.statusRec : Theme.borderSubtle
                                    border.width: 1

                                    RowLayout {
                                        anchors.centerIn: parent
                                        spacing: 4
                                        Text { text: rowDelegate.modelData.is_locked ? "🔒" : "🔓"; font.pixelSize: 10 }
                                        Text {
                                            text: rowDelegate.modelData.is_locked ? "Locked" : "Normal"
                                            color: rowDelegate.modelData.is_locked ? Theme.statusRec : Theme.textSecondary
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.toggleLock(rowDelegate.modelData)
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                // Play Button
                                Rectangle {
                                    Layout.preferredWidth: 54
                                    Layout.preferredHeight: 24
                                    radius: Theme.radiusSm
                                    color: Theme.accentPrimary

                                    Text {
                                        anchors.centerIn: parent
                                        text: "Play"
                                        color: Theme.accentPrimaryFg
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.family: Theme.fontSans
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.playItem(rowDelegate.modelData)
                                    }
                                }
                            }

                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                            }
                        }
                    }
                }
            }
        }
    }

    ClipExportModal {
        id: clipExportModal
    }
}

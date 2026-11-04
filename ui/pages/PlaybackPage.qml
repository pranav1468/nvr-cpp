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
    property bool autoNext: false
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

    function getEpochMs(isoOrMs) {
        if (typeof isoOrMs === "number" && !isNaN(isoOrMs)) return isoOrMs;
        if (!isoOrMs) return 0;
        var d = new Date(isoOrMs);
        return isNaN(d.getTime()) ? 0 : d.getTime();
    }

    function formatTimeMSS(sec, forceHours) {
        var s = Math.max(0, Math.floor(sec || 0));
        var hrs = Math.floor(s / 3600);
        var mins = Math.floor((s % 3600) / 60);
        var rem = s % 60;
        var pad = function(n) { return (n < 10 ? "0" : "") + n; };
        if (hrs > 0 || forceHours) {
            return pad(hrs) + ":" + pad(mins) + ":" + pad(rem);
        }
        return pad(mins) + ":" + pad(rem);
    }

    function formatLocalDate(iso) {
        if (!iso) return "";
        var d = new Date(iso);
        if (isNaN(d.getTime())) return iso.substr(0, 10);
        return d.toLocaleDateString();
    }

    function formatLocalTimeRange(startIso, endIso) {
        if (!startIso) return "00:00 ~ 00:00";
        var d1 = new Date(startIso);
        var d2 = new Date(endIso);
        if (isNaN(d1.getTime())) return (startIso.substr(11, 5) + " ~ " + (endIso ? endIso.substr(11, 5) : ""));
        var pad = function(n) { return (n < 10 ? "0" : "") + n; };
        var t1 = pad(d1.getHours()) + ":" + pad(d1.getMinutes());
        var t2 = isNaN(d2.getTime()) ? "" : (pad(d2.getHours()) + ":" + pad(d2.getMinutes()));
        return t1 + (t2 ? " – " + t2 : "");
    }

    function getActiveItemTitle() {
        if (!root.activeItem) return "No Footage Selected";
        var ch = root.activeItem.channel_id || 1;
        var startIso = root.activeItem.start_time || "";
        var d = new Date(startIso);
        var timeStr = "";
        if (!isNaN(d.getTime())) {
            var pad = function(n) { return (n < 10 ? "0" : "") + n; };
            timeStr = pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
        } else {
            timeStr = startIso.length >= 19 ? startIso.substr(11, 8) : startIso;
        }
        return "Camera " + (ch < 10 ? "0" : "") + ch + " · " + timeStr + " (" + formatDurationCompact(root.totalDurationSec) + ")";
    }

    function formatTimeHMS(sec) {
        return formatTimeMSS(sec);
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
        audioOutput: AudioOutput { id: playerAudioOutput }
        playbackRate: root.playbackRate
        loops: root.loopCurrent ? MediaPlayer.Infinite : MediaPlayer.Once

        onPositionChanged: {
            root.currentPositionSec = mediaPlayer.position / 1000.0;
            // Bounded clip playback: stop cleanly when clip finishes
            if (root.totalDurationSec > 0 && !root.autoNext && !root.loopCurrent && root.currentPositionSec >= root.totalDurationSec) {
                mediaPlayer.pause();
                root.isPlaying = false;
            }
        }

        onDurationChanged: {
            if (mediaPlayer.duration > 0) {
                root.totalDurationSec = mediaPlayer.duration / 1000.0;
            }
        }

        onPlaybackStateChanged: {
            root.isPlaying = (mediaPlayer.playbackState === MediaPlayer.PlayingState);
            if (mediaPlayer.playbackState === MediaPlayer.StoppedState) {
                if (root.autoNext && !root.loopCurrent) {
                    root.advanceToNextSequentialSegment();
                } else {
                    root.isPlaying = false;
                }
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
                                                text: root.formatLocalDate(cardDelegate.modelData.start_time) + " · " + root.formatLocalTimeRange(cardDelegate.modelData.start_time, cardDelegate.modelData.end_time)
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

                    // 16:9 Dedicated Video Surface with Top "You're Watching" Overlay
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

                        // Top "You're Watching" Overlay Header
                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 52
                            color: Qt.rgba(0, 0, 0, 0.75)
                            z: 20
                            visible: root.activeItem !== null

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                spacing: 12

                                // Back button to activity cards
                                Rectangle {
                                    Layout.preferredHeight: 30
                                    Layout.preferredWidth: backBtnRow.implicitWidth + 18
                                    radius: 6
                                    color: Qt.rgba(1, 1, 1, 0.15)
                                    border.color: Qt.rgba(1, 1, 1, 0.2)
                                    border.width: 1

                                    RowLayout {
                                        id: backBtnRow
                                        anchors.centerIn: parent
                                        spacing: 6
                                        AppIcon {
                                            name: "back"
                                            size: 13
                                            color: "#ffffff"
                                            strokeWidth: 2
                                        }
                                        Text {
                                            text: "Back"
                                            color: "#ffffff"
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                            font.family: Theme.fontSans
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            mediaPlayer.pause();
                                            root.isPlaying = false;
                                            root.viewMode = "cards";
                                        }
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                // Center "You're Watching" Title
                                ColumnLayout {
                                    spacing: 1
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "YOU'RE WATCHING"
                                        color: "#94a3b8"
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.letterSpacing: 1.0
                                        font.family: Theme.fontSans
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: root.getActiveItemTitle()
                                        color: "#ffffff"
                                        font.pixelSize: 13
                                        font.bold: true
                                        font.family: Theme.fontSans
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                // Right Playing / Paused badge
                                Rectangle {
                                    Layout.preferredHeight: 24
                                    Layout.preferredWidth: playBadgeRow.implicitWidth + 14
                                    radius: 6
                                    color: root.isPlaying ? Qt.rgba(0.85, 0.2, 0.2, 0.95) : Qt.rgba(0, 0, 0, 0.6)
                                    border.color: root.isPlaying ? "transparent" : Qt.rgba(1, 1, 1, 0.2)
                                    border.width: 1

                                    RowLayout {
                                        id: playBadgeRow
                                        anchors.centerIn: parent
                                        spacing: 5
                                        Rectangle {
                                            width: 6
                                            height: 6
                                            radius: 3
                                            color: "#ffffff"
                                            visible: root.isPlaying
                                        }
                                        Text {
                                            text: root.isPlaying ? "PLAYING" : "PAUSED"
                                            color: "#ffffff"
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: Theme.fontMono
                                        }
                                    }
                                }
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

                    // Modern Full-Width Scrubber & Transport Controls
                    Rectangle {
                        Layout.fillWidth: true
                        height: 76
                        color: Theme.bgSecondary
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: Theme.radiusMd

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 6

                            // Full-Width Modern Scrubber Track
                            Item {
                                id: modernScrubTrack
                                Layout.fillWidth: true
                                height: 18

                                // Background track
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    height: 6
                                    radius: 3
                                    color: Qt.rgba(1, 1, 1, 0.2)

                                    // Fill progress
                                    Rectangle {
                                        height: parent.height
                                        radius: 3
                                        color: Theme.accentPrimary
                                        width: Math.max(0, Math.min(parent.width, (root.currentPositionSec / Math.max(1, root.totalDurationSec)) * parent.width))
                                    }
                                }

                                // Circular Scrubber Knob
                                Rectangle {
                                    id: scrubKnob
                                    width: 14
                                    height: 14
                                    radius: 7
                                    color: "#ffffff"
                                    border.color: Qt.rgba(0, 0, 0, 0.3)
                                    border.width: 1
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: Math.max(0, Math.min(modernScrubTrack.width - width, (root.currentPositionSec / Math.max(1, root.totalDurationSec)) * modernScrubTrack.width - width / 2))
                                }

                                MouseArea {
                                    id: scrubMouseArea
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    preventStealing: true

                                    function updateSeek(mouseX) {
                                        if (modernScrubTrack.width <= 0) return;
                                        var ratio = Math.max(0.0, Math.min(1.0, mouseX / modernScrubTrack.width));
                                        var targetSec = ratio * root.totalDurationSec;
                                        mediaPlayer.position = targetSec * 1000.0;
                                        root.currentPositionSec = targetSec;
                                    }

                                    onClicked: function(mouse) {
                                        updateSeek(mouse.x);
                                    }
                                    onPositionChanged: function(mouse) {
                                        if (pressed) {
                                            updateSeek(mouse.x);
                                        }
                                    }
                                }
                            }

                            // Transport Controls Row
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                // Left Controls
                                RowLayout {
                                    spacing: 8

                                    // Play/Pause circular button
                                    Rectangle {
                                        width: 34
                                        height: 34
                                        radius: 17
                                        color: "#ffffff"

                                        Text {
                                            anchors.centerIn: parent
                                            text: root.isPlaying ? "❚❚" : "▶"
                                            color: "#000000"
                                            font.pixelSize: 13
                                            leftPadding: root.isPlaying ? 0 : 2
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

                                    // -10s Skip button
                                    Rectangle {
                                        width: 32
                                        height: 32
                                        radius: 16
                                        color: Qt.rgba(1, 1, 1, 0.1)

                                        RowLayout {
                                            anchors.centerIn: parent
                                            spacing: 1
                                            Text {
                                                text: "⟲"
                                                color: "#ffffff"
                                                font.pixelSize: 13
                                            }
                                            Text {
                                                text: "10"
                                                color: "#ffffff"
                                                font.pixelSize: 9
                                                font.bold: true
                                                font.family: Theme.fontMono
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: mediaPlayer.position = Math.max(0, mediaPlayer.position - 10000)
                                        }
                                    }

                                    // +10s Skip button
                                    Rectangle {
                                        width: 32
                                        height: 32
                                        radius: 16
                                        color: Qt.rgba(1, 1, 1, 0.1)

                                        RowLayout {
                                            anchors.centerIn: parent
                                            spacing: 1
                                            Text {
                                                text: "10"
                                                color: "#ffffff"
                                                font.pixelSize: 9
                                                font.bold: true
                                                font.family: Theme.fontMono
                                            }
                                            Text {
                                                text: "⟳"
                                                color: "#ffffff"
                                                font.pixelSize: 13
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: mediaPlayer.position = Math.min(mediaPlayer.duration, mediaPlayer.position + 10000)
                                        }
                                    }

                                    // Audio Mute / Unmute
                                    Rectangle {
                                        width: 32
                                        height: 32
                                        radius: 16
                                        color: Qt.rgba(1, 1, 1, 0.1)

                                        AppIcon {
                                            anchors.centerIn: parent
                                            name: playerAudioOutput.muted ? "volume_mute" : "volume"
                                            size: 14
                                            color: "#ffffff"
                                            strokeWidth: 2
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: playerAudioOutput.muted = !playerAudioOutput.muted
                                        }
                                    }

                                    // Clean Duration Counter: 00:04 / 01:00
                                    Text {
                                        text: {
                                            var hasH = root.totalDurationSec >= 3600;
                                            return root.formatTimeMSS(root.currentPositionSec, hasH) + " / " + root.formatTimeMSS(root.totalDurationSec, hasH);
                                        }
                                        color: "#ffffff"
                                        font.pixelSize: 12
                                        font.bold: true
                                        font.family: Theme.fontMono
                                        leftPadding: 6
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                // Right Controls
                                RowLayout {
                                    spacing: 8

                                    // Speed Pills
                                    RowLayout {
                                        spacing: 2
                                        Repeater {
                                            model: [0.5, 1.0, 2.0, 4.0]
                                            Rectangle {
                                                id: rateBtn
                                                required property var modelData
                                                Layout.preferredHeight: 26
                                                Layout.preferredWidth: 34
                                                radius: 6
                                                color: root.playbackRate === rateBtn.modelData ? "#ffffff" : Qt.rgba(0, 0, 0, 0.4)
                                                border.color: Qt.rgba(1, 1, 1, 0.15)
                                                border.width: 1

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: rateBtn.modelData + "x"
                                                    color: root.playbackRate === rateBtn.modelData ? "#000000" : "#ffffff"
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

                                    // Export Clip Button
                                    Rectangle {
                                        Layout.preferredHeight: 28
                                        Layout.preferredWidth: exportBtnRow.implicitWidth + 16
                                        radius: 6
                                        color: Qt.rgba(1, 1, 1, 0.1)
                                        border.color: Qt.rgba(1, 1, 1, 0.2)
                                        border.width: 1

                                        RowLayout {
                                            id: exportBtnRow
                                            anchors.centerIn: parent
                                            spacing: 5
                                            Text {
                                                text: "⤓"
                                                color: "#ffffff"
                                                font.pixelSize: 12
                                                font.bold: true
                                            }
                                            Text {
                                                text: "Export Clip"
                                                color: "#ffffff"
                                                font.pixelSize: 11
                                                font.weight: Font.Medium
                                                font.family: Theme.fontSans
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.openExportDialog(root.activeItem)
                                        }
                                    }
                                }
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

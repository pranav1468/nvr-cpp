import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root

    property string traceLogText: "Connecting to in-process diagnostics tracer..."
    property bool autoTail: true
    property string storageMountPoint: "./recordings"
    property string storageStatusText: "Ready"
    property bool storageWritable: true
    property string storageDiskHealth: "OK (write throttling ready)"
    property bool isAdmin: ApiClient.currentUserRole === "ADMIN"
    property var usersList: []

    Component.onCompleted: {
        loadAll();
    }

    Timer {
        id: traceTimer
        interval: 2500
        repeat: true
        running: root.autoTail
        onTriggered: root.fetchTrace()
    }

    function loadAll() {
        fetchTrace();
        fetchStorage();
        if (root.isAdmin) {
            fetchUsers();
        }
    }

    function fetchUsers() {
        ApiClient.get("/api/v1/users", function(res) {
            var data = (res && res.data) ? res.data : res;
            if (data && Array.isArray(data)) {
                root.usersList = data;
            }
        });
    }

    function fetchStorage() {
        ApiClient.get("/api/v1/storage", function(res) {
            var data = (res && res.data) ? res.data : res;
            if (data && data.recording_path) {
                root.storageMountPoint = data.recording_path;
                root.storageWritable = (data.is_writable !== false);
                root.storageStatusText = root.storageWritable ? "Ready (Writable)" : "Read-Only / Error";
                if (data.free_storage_bytes !== undefined && data.total_storage_bytes !== undefined) {
                    var freeGb = (data.free_storage_bytes / (1024 * 1024 * 1024)).toFixed(1);
                    var totalGb = (data.total_storage_bytes / (1024 * 1024 * 1024)).toFixed(1);
                    root.storageDiskHealth = freeGb + " GB Free / " + totalGb + " GB (" + (root.storageWritable ? "Writable" : "Read-Only") + ")";
                }
            }
        });
    }

    function fetchTrace() {
        ApiClient.getDiagnostics(function(res) {
            var data = (res && res.data) ? res.data : res;
            if (data && data.breadcrumbs && Array.isArray(data.breadcrumbs) && data.breadcrumbs.length > 0) {
                var lines = [];
                for (var i = 0; i < data.breadcrumbs.length; ++i) {
                    var b = data.breadcrumbs[i];
                    var dateStr = "";
                    if (b.timestamp_us) {
                        var d = new Date(Math.floor(b.timestamp_us / 1000));
                        var hh = ("0" + d.getHours()).slice(-2);
                        var mm = ("0" + d.getMinutes()).slice(-2);
                        var ss = ("0" + d.getSeconds()).slice(-2);
                        var ms = ("00" + d.getMilliseconds()).slice(-3);
                        dateStr = hh + ":" + mm + ":" + ss + "." + ms;
                    } else {
                        dateStr = new Date().toISOString().slice(11, 23);
                    }
                    var lvl = (b.level ? b.level : "INFO");
                    while (lvl.length < 5) lvl += " ";
                    var tag = b.tag ? ("[" + b.tag + "]") : "";
                    var loc = b.file ? (" (" + b.file + ":" + (b.line || 0) + ")") : "";
                    lines.push("[" + dateStr + "] [" + lvl + "] " + tag + " " + (b.message || "") + loc);
                }
                root.traceLogText = lines.join("\n");

                var proc = data.process || data.process_metrics;
                if (proc) {
                    var rssMb = proc.rss_kb ? (proc.rss_kb / 1024).toFixed(1) : "0.0";
                    var threads = proc.threads !== undefined ? proc.threads : 0;
                    traceBadgeText.text = lines.length + " events · RSS: " + rssMb + " MB (" + threads + " threads)";
                } else {
                    traceBadgeText.text = lines.length + " events";
                }
            } else {
                root.traceLogText = "No active trace breadcrumbs recorded on local NVR instance.";
                traceBadgeText.text = "0 events · Inactive";
            }
        }, function(err) {
            root.traceLogText = "[Trace Error] " + String(err);
        });
    }

    ScrollView {
        id: sysScrollView
        anchors.fill: parent
        anchors.margins: 20
        clip: true

        ColumnLayout {
            width: root.width - 40
            spacing: 20

            // 1. Header (Exact Web Parity)
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    Text {
                        text: "System hardware & storage health"
                        color: Theme.textPrimary
                        font.pixelSize: 22
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                    Text {
                        text: "NXP i.MX 95 target parameters · zero-DB storage · diagnostics log"
                        color: Theme.textMuted
                        font.pixelSize: 13
                        font.family: Theme.fontSans
                    }
                }

                // Refresh Button
                Rectangle {
                    Layout.preferredHeight: 32
                    Layout.preferredWidth: refreshSysRow.implicitWidth + 24
                    color: Theme.bgSecondary
                    border.color: Theme.borderSubtle
                    radius: 6

                    RowLayout {
                        id: refreshSysRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "refresh"
                            size: 13
                            color: Theme.textSecondary
                            strokeWidth: 2
                        }

                        Text {
                            text: "Refresh telemetry"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.loadAll()
                    }
                }
            }

            // 2. Hardware Specs & Storage Retention Cards (Exact Web Parity)
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                // Target Platform Architecture Card
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 190
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 16
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 10

                        Text {
                            text: "Target platform architecture (i.MX 95)"
                            color: Theme.textPrimary
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontSans
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.borderSubtle
                        }

                        GridLayout {
                            columns: 2
                            Layout.fillWidth: true
                            rowSpacing: 8
                            columnSpacing: 16

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Processor target"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "6x Cortex-A55 @ 1.8GHz"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Memory budget"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "4096 MB LPDDR5 (bound < 5.3MB)"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "AI acceleration"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "Neutron NPU (8 eTOPS INT8)"; color: Theme.statusOnline; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "VPU decoding"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "V4L2 H.264 / H.265 pass-through"; color: Theme.accentPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Compositor decoupling"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "IDisplayBackend / QQuickProvider"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Ingress architecture"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "1 RTSP ingress / camera"; color: Theme.statusOnline; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                        }
                    }
                }

                // Dedicated Media Storage & Retention Card (Exact Parity with Target Platform Architecture Card)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 190
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: 16
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Dedicated media storage & filesystem retention"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontSans
                                Layout.fillWidth: true
                            }

                            Rectangle {
                                id: btnChangeStoragePath
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: changeStorageRow.implicitWidth + 20
                                color: Theme.bgSecondary
                                border.color: Theme.borderSubtle
                                radius: 6

                                RowLayout {
                                    id: changeStorageRow
                                    anchors.centerIn: parent
                                    spacing: 5

                                    AppIcon {
                                        name: "edit"
                                        size: 12
                                        color: Theme.textSecondary
                                        strokeWidth: 2
                                    }

                                    Text {
                                        text: "Change Path"
                                        color: Theme.textSecondary
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                        font.family: Theme.fontSans
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: storagePathModal.openModal()
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Theme.borderSubtle
                        }

                        GridLayout {
                            columns: 2
                            Layout.fillWidth: true
                            rowSpacing: 8
                            columnSpacing: 16

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Storage mount point"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: root.storageMountPoint + " (dedicated disk)"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Metadata storage"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "Filesystem direct (.mp4 + .thumb)"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Segment strategy"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "Two-phase atomic .tmp to .mp4"; color: Theme.accentPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Retention policy"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "Circular FIFO file scans"; color: Theme.statusWarning; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Zero-transcode rule"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "100% stream copy (re-encode proh..."; color: Theme.statusOnline; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Disk health"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: root.storageDiskHealth; color: Theme.statusOnline; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }

            // 3. Deep Trace Diagnostics Card (Exact Web Parity)
            Rectangle {
                Layout.fillWidth: true
                height: 330
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 16

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            text: "In-process deep trace log stream"
                            color: Theme.textPrimary
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        Rectangle {
                            height: 22
                            width: traceBadgeText.implicitWidth + 14
                            radius: 6
                            color: Theme.bgSecondary

                            Text {
                                id: traceBadgeText
                                anchors.centerIn: parent
                                text: "54 events · RSS: 14.1 MB (100 threads)"
                                color: Theme.textSecondary
                                font.pixelSize: 10
                                font.bold: true
                                font.family: Theme.fontMono
                            }
                        }

                        Item { Layout.fillWidth: true }

                        Item {
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            Layout.preferredWidth: autoTailRow.implicitWidth
                            Layout.preferredHeight: 24

                            RowLayout {
                                id: autoTailRow
                                anchors.fill: parent
                                spacing: 8

                                Rectangle {
                                    width: 16
                                    height: 16
                                    radius: 4
                                    color: root.autoTail ? Theme.accentPrimary : "transparent"
                                    border.color: root.autoTail ? Theme.accentPrimary : Theme.borderLight
                                    border.width: 1

                                    Text {
                                        anchors.centerIn: parent
                                        text: "✓"
                                        color: "#ffffff"
                                        font.pixelSize: 11
                                        font.bold: true
                                        visible: root.autoTail
                                    }
                                }

                                Text {
                                    text: "Auto-tail (2s)"
                                    font.pixelSize: 12
                                    font.family: Theme.fontSans
                                    color: Theme.textSecondary
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.autoTail = !root.autoTail
                            }
                        }
                    }

                    // Trace terminal box
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#0f172a"
                        radius: 12
                        clip: true

                        Flickable {
                            id: traceFlick
                            anchors.fill: parent
                            anchors.margins: 12
                            contentWidth: traceText.width
                            contentHeight: traceText.height
                            clip: true

                            Text {
                                id: traceText
                                width: traceFlick.width
                                text: root.traceLogText
                                color: "#cbd5e1"
                                font.family: Theme.fontMono
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }
                    }

                    Text {
                        text: "The live engine streams this trace from its in-process diagnostics tracer; the console updates continuously."
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }
                }
            }
        }
    }

    // Recording Storage Location Modal Dialog (Exact Parity with Web modal-change-storage)
    Rectangle {
        id: storagePathModal
        anchors.fill: parent
        color: "#000000bb"
        visible: false
        z: 9999

        property string errorText: ""

        function openModal() {
            errorText = "";
            newPathInput.text = root.storageMountPoint;
            visible = true;
            newPathInput.forceActiveFocus();
        }

        function closeModal() {
            visible = false;
        }

        function applyPath() {
            var newP = newPathInput.text.trim();
            if (!newP) {
                storagePathModal.errorText = "Please enter a valid storage path";
                return;
            }
            ApiClient.post("/api/v1/storage/path", {path: newP}, function(res) {
                var d = (res && res.data) ? res.data : res;
                root.storageMountPoint = (d && d.recording_path) || d.path || newP;
                root.storageWritable = true;
                root.storageStatusText = "Ready (Writable)";
                storagePathModal.closeModal();
                root.loadAll();
            }, function(err) {
                storagePathModal.errorText = "Failed: " + String(err);
                root.storageWritable = false;
            });
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {} // Prevents clicking through modal backdrop
        }

        Rectangle {
            anchors.centerIn: parent
            width: 480
            implicitHeight: storageModalCol.implicitHeight + 40
            color: Theme.bgSurface
            border.color: Theme.borderSubtle
            radius: Theme.radiusLg
            clip: true

            ColumnLayout {
                id: storageModalCol
                anchors.fill: parent
                anchors.margins: 20
                spacing: 14

                // Header
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: "Recording Storage Location"
                            color: Theme.textPrimary
                            font.pixelSize: 16
                            font.bold: true
                            font.family: Theme.fontDisplay
                        }
                        Text {
                            text: "Set the active drive partition or folder for camera recordings."
                            color: Theme.textMuted
                            font.pixelSize: 12
                            font.family: Theme.fontSans
                        }
                    }

                    Rectangle {
                        width: 28
                        height: 28
                        color: Theme.bgInput
                        radius: 6
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: storagePathModal.closeModal()
                        }
                    }
                }

                // Error Banner
                Rectangle {
                    Layout.fillWidth: true
                    height: 32
                    radius: 6
                    color: "#2a1515"
                    border.color: "#ef4444"
                    visible: storagePathModal.errorText.length > 0

                    Text {
                        anchors.fill: parent
                        anchors.margins: 8
                        text: storagePathModal.errorText
                        color: "#ef4444"
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                        elide: Text.ElideRight
                    }
                }

                // Current Active Path
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "Current Active Path"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        color: Theme.bgInput
                        border.color: Theme.borderSubtle
                        radius: 6

                        Text {
                            anchors.fill: parent
                            anchors.margins: 10
                            text: root.storageMountPoint
                            color: Theme.textMuted
                            font.family: Theme.fontMono
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }

                // New Storage Mount Directory
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "New Storage Mount Directory"
                        color: Theme.textSecondary
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontSans
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 36
                        color: Theme.bgSecondary
                        border.color: Theme.borderLight
                        radius: 6

                        TextInput {
                            id: newPathInput
                            anchors.fill: parent
                            anchors.margins: 10
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: 12
                            selectByMouse: true
                            clip: true
                            onAccepted: storagePathModal.applyPath()
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Point camera recordings to an attached SATA HDD, NVMe SSD, or external USB drive. Path will be verified for write permissions before applying."
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        font.family: Theme.fontSans
                    }
                }

                // Actions
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        width: 80
                        height: 34
                        color: Theme.bgSecondary
                        radius: 6
                        border.color: Theme.borderSubtle

                        Text {
                            anchors.centerIn: parent
                            text: "Cancel"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: storagePathModal.closeModal()
                        }
                    }

                    Rectangle {
                        width: 100
                        height: 34
                        color: Theme.accentPrimary
                        radius: 6

                        Text {
                            anchors.centerIn: parent
                            text: "Apply Path"
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: storagePathModal.applyPath()
                        }
                    }
                }
            }
        }
    }
}

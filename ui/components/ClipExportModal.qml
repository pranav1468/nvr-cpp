import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root

    property bool isOpen: false
    property string channelId: "1"
    property string startTimeIso: ""
    property string endTimeIso: ""
    property string filename: ""
    property string jobId: ""
    property string jobState: ""
    property int progressPercent: 0
    property string statusMessage: ""
    property string outputPath: ""
    property bool isExporting: false

    signal closed()

    anchors.fill: parent
    color: "#000000bb"
    visible: isOpen
    z: 9999

    MouseArea {
        anchors.fill: parent
        onClicked: {}
    }

    function open(item) {
        if (item) {
            root.channelId = String(item.channel_id || "1");
            root.startTimeIso = item.start_time || "";
            root.endTimeIso = item.end_time || "";
            var ts = (item.start_time || "clip").replace(/[-:TZ]/g, "");
            root.filename = "incident_cam" + root.channelId + "_" + ts + ".mp4";
        } else {
            root.channelId = "1";
            var now = new Date();
            root.startTimeIso = now.toISOString();
            root.endTimeIso = new Date(now.getTime() + 300000).toISOString();
            root.filename = "export_incident.mp4";
        }

        channelField.text = root.channelId;
        startField.text = root.startTimeIso;
        endField.text = root.endTimeIso;
        fileField.text = root.filename;

        root.jobId = "";
        root.jobState = "";
        root.progressPercent = 0;
        root.statusMessage = "";
        root.outputPath = "";
        root.isExporting = false;
        root.isOpen = true;
    }

    function close() {
        pollTimer.stop();
        root.isOpen = false;
        root.closed();
    }

    function startExport() {
        var sIso = startField.text.trim();
        var eIso = endField.text.trim();
        var sMs = new Date(sIso).getTime();
        var eMs = new Date(eIso).getTime();

        if (isNaN(sMs) || sMs <= 0 || isNaN(eMs) || eMs <= 0) {
            root.statusMessage = "Error: Invalid ISO 8601 date string (e.g. 2026-11-02T10:00:00Z)";
            return;
        }

        if (eMs <= sMs) {
            root.statusMessage = "Error: End time must be greater than start time";
            return;
        }

        root.isExporting = true;
        root.statusMessage = "Initiating in-process stream-copy export...";
        root.progressPercent = 15;
        root.jobState = "RUNNING";

        var payload = {
            channel_id: parseInt(channelField.text.trim(), 10) || 1,
            start_time_ms: sMs,
            end_time_ms: eMs,
            filename: fileField.text.trim()
        };

        ApiClient.exportClip(payload, function(res) {
            var jid = (res && res.data && res.data.job_id) ? res.data.job_id : ((res && res.job_id) ? res.job_id : "");
            if (jid) {
                root.jobId = jid;
                root.progressPercent = 40;
                root.statusMessage = "Concatenating MP4 fragments (Zero transcoding)...";
                pollTimer.start();
            } else {
                root.isExporting = false;
                root.statusMessage = "Export failed: " + (res && res.message ? res.message : "Unknown error");
            }
        }, function(err) {
            root.isExporting = false;
            root.statusMessage = "Export failed: " + err;
        });
    }

    Timer {
        id: pollTimer
        interval: 600
        repeat: true
        running: false
        onTriggered: {
            if (!root.jobId) {
                stop();
                return;
            }
            ApiClient.getExportStatus(root.jobId, function(res) {
                var d = (res && res.data) ? res.data : res;
                if (d) {
                    root.jobState = d.state || "";
                    if (root.jobState === "COMPLETED") {
                        root.progressPercent = 100;
                        root.outputPath = d.output_file || "";
                        root.statusMessage = "Clip successfully exported!";
                        root.isExporting = false;
                        pollTimer.stop();
                    } else if (root.jobState === "FAILED") {
                        root.isExporting = false;
                        root.statusMessage = "Export failed: " + (d.error || "Concat error");
                        pollTimer.stop();
                    } else {
                        root.progressPercent = Math.min(95, root.progressPercent + 20);
                        root.statusMessage = "Stitching fragments: " + (d.segments_stitched || 0) + " segments...";
                    }
                }
            }, function(err) {
                console.warn("Poll export error: " + err);
            });
        }
    }

    // Modal Content Box
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 40)
        height: contentCol.implicitHeight + 48
        radius: Theme.radiusLg
        color: Theme.bgModal
        border.color: Theme.borderSubtle
        border.width: 1

        ColumnLayout {
            id: contentCol
            anchors.fill: parent
            anchors.margins: 24
            spacing: 16

            // Header Row
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Export Recorded Clip"
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontDisplay
                    color: Theme.textPrimary
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "✕"
                    font.pixelSize: 16
                    color: Theme.textMuted
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }

            Text {
                text: "Zero-transcode lossless stream-copy concatenates atomic 1-minute MP4 segments into a standalone evidence file."
                font.pixelSize: 12
                color: Theme.textSecondary
                font.family: Theme.fontSans
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.borderSubtle
            }

            // Input Fields Grid
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 12
                Layout.fillWidth: true

                Text {
                    text: "Camera Channel:"
                    font.pixelSize: 12
                    font.bold: true
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                }
                TextField {
                    id: channelField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    font.pixelSize: 12
                    color: Theme.textPrimary
                    placeholderText: "Channel ID (e.g. 1)"
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: Theme.bgSecondary
                        border.color: channelField.activeFocus ? Theme.accentPrimary : Theme.borderSubtle
                        border.width: 1
                    }
                }

                Text {
                    text: "Start UTC Time:"
                    font.pixelSize: 12
                    font.bold: true
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                }
                TextField {
                    id: startField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    font.pixelSize: 12
                    color: Theme.textPrimary
                    placeholderText: "YYYY-MM-DDTHH:MM:SSZ"
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: Theme.bgSecondary
                        border.color: startField.activeFocus ? Theme.accentPrimary : Theme.borderSubtle
                        border.width: 1
                    }
                }

                Text {
                    text: "End UTC Time:"
                    font.pixelSize: 12
                    font.bold: true
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                }
                TextField {
                    id: endField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    font.pixelSize: 12
                    color: Theme.textPrimary
                    placeholderText: "YYYY-MM-DDTHH:MM:SSZ"
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: Theme.bgSecondary
                        border.color: endField.activeFocus ? Theme.accentPrimary : Theme.borderSubtle
                        border.width: 1
                    }
                }

                Text {
                    text: "File Name:"
                    font.pixelSize: 12
                    font.bold: true
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                }
                TextField {
                    id: fileField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    font.pixelSize: 12
                    color: Theme.textPrimary
                    placeholderText: "clip_output.mp4"
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: Theme.bgSecondary
                        border.color: fileField.activeFocus ? Theme.accentPrimary : Theme.borderSubtle
                        border.width: 1
                    }
                }
            }

            // Progress Bar & Status (when active or complete)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: root.isExporting || root.progressPercent > 0 || root.statusMessage.length > 0

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: root.statusMessage
                        font.pixelSize: 12
                        font.bold: true
                        color: root.jobState === "FAILED" ? Theme.statusDanger : (root.jobState === "COMPLETED" ? Theme.statusOnline : Theme.textAccent)
                        font.family: Theme.fontSans
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    Text {
                        text: root.progressPercent + "%"
                        font.pixelSize: 11
                        font.bold: true
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        visible: root.isExporting || root.jobState === "COMPLETED"
                    }
                }

                // Progress Bar Track
                Rectangle {
                    Layout.fillWidth: true
                    height: 6
                    radius: 3
                    color: Theme.bgSecondary
                    clip: true

                    Rectangle {
                        height: parent.height
                        width: parent.width * (root.progressPercent / 100.0)
                        radius: 3
                        color: root.jobState === "FAILED" ? Theme.statusDanger : Theme.accentPrimary
                        Behavior on width { NumberAnimation { duration: 250 } }
                    }
                }

                // Output File Path Display
                Text {
                    text: root.outputPath ? ("Saved to: " + root.outputPath) : ""
                    font.pixelSize: 11
                    color: Theme.textMuted
                    font.family: Theme.fontMono
                    visible: root.outputPath.length > 0
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                }
            }

            // Button Row
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Item { Layout.fillWidth: true }

                // Cancel Button
                Rectangle {
                    Layout.preferredWidth: 90
                    Layout.preferredHeight: 34
                    radius: Theme.radiusSm
                    color: Theme.bgSecondary
                    border.color: Theme.borderSubtle
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: root.jobState === "COMPLETED" ? "Close" : "Cancel"
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }

                // Export Button
                Rectangle {
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 34
                    radius: Theme.radiusSm
                    color: root.isExporting ? Theme.textMuted : Theme.accentPrimary
                    enabled: !root.isExporting

                    Text {
                        anchors.centerIn: parent
                        text: root.isExporting ? "Exporting..." : "Start Export"
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.accentPrimaryFg
                        font.family: Theme.fontSans
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: root.isExporting ? Qt.ArrowCursor : Qt.PointingHandCursor
                        onClicked: {
                            if (!root.isExporting) {
                                root.startExport();
                            }
                        }
                    }
                }
            }
        }
    }
}

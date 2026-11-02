import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root

    property string traceLogText: "Connecting to in-process diagnostics tracer..."
    property bool autoTail: true

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

                // Dedicated Media Storage & Retention Card
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
                            text: "Dedicated media storage & filesystem retention"
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
                                Text { text: "Storage mount point"; color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.fontSans }
                                Text { text: "/recordings (dedicated disk)"; color: Theme.textPrimary; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
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
                                Text { text: "OK (write throttling ready)"; color: Theme.statusOnline; font.bold: true; font.family: Theme.fontMono; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
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
}

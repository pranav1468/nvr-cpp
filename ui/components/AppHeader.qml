import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

/**
 * Enterprise Application Header Component
 * Pixel-exact parity with web console (web/index.html & web/app.css)
 */
Rectangle {
    id: root
    height: 56
    color: Theme.bgSurface
    border.color: Theme.borderSubtle
    border.width: 1

    property string coreStatus: "ONLINE"
    property string cpuLoad: "0.0%"
    property string ramFree: "-- MB"
    property string storageFree: "-- GB"

    Component.onCompleted: {
        ApiClient.telemetryUpdated.connect(updateTelemetry);
        ApiClient.getSystemStatus(updateTelemetry);
    }

    function updateTelemetry(data) {
        if (!data) return;
        if (data.status) root.coreStatus = data.status;
        else if (data.core_status) root.coreStatus = data.core_status;

        // CPU load calculation matching web/app.js
        if (typeof data.cpu_load_1m === "number") {
            var cpuPct = Math.min(100, (data.cpu_load_1m / 6.0) * 100);
            root.cpuLoad = cpuPct.toFixed(1) + "%";
        } else if (data.cpu_usage_pct !== undefined) {
            root.cpuLoad = Number(data.cpu_usage_pct).toFixed(1) + "%";
        } else if (data.cpu_load !== undefined) {
            root.cpuLoad = String(data.cpu_load);
        }

        // RAM free calculation matching web/app.js
        if (data.ram_free_bytes) {
            var ramMb = Math.round(data.ram_free_bytes / (1024 * 1024));
            root.ramFree = ramMb + " MB";
        } else if (data.ram_free_mb !== undefined) {
            root.ramFree = data.ram_free_mb + " MB";
        } else if (data.ram_free !== undefined) {
            root.ramFree = String(data.ram_free);
        }

        // Storage free calculation matching web/app.js
        if (data.free_storage_bytes) {
            var storageGb = (data.free_storage_bytes / (1024 * 1024 * 1024)).toFixed(1);
            root.storageFree = storageGb + " GB";
        } else if (data.storage_free_gb !== undefined) {
            root.storageFree = Number(data.storage_free_gb).toFixed(1) + " GB";
        } else if (data.storage_free !== undefined) {
            root.storageFree = String(data.storage_free);
        }
    }

    // Telemetry polling timer
    Timer {
        interval: 3000
        running: true
        repeat: true
        onTriggered: ApiClient.getSystemStatus(root.updateTelemetry)
    }

    // 1-second clock timer
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            var now = new Date();
            var y = now.getFullYear();
            var m = ("0" + (now.getMonth() + 1)).slice(-2);
            var d = ("0" + now.getDate()).slice(-2);
            var hh = ("0" + now.getHours()).slice(-2);
            var mm = ("0" + now.getMinutes()).slice(-2);
            var ss = ("0" + now.getSeconds()).slice(-2);
            clockLabel.text = y + "-" + m + "-" + d + " " + hh + ":" + mm + ":" + ss;
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 16

        // Brandmark (Blue rounded square + Typography)
        RowLayout {
            spacing: 10
            Layout.alignment: Qt.AlignVCenter

            Rectangle {
                Layout.preferredHeight: 36
                Layout.preferredWidth: 36
                radius: 10
                color: Theme.accentPrimary

                AppIcon {
                    anchors.centerIn: parent
                    name: "video"
                    size: 18
                    color: "#ffffff"
                    strokeWidth: 2
                }
            }

            ColumnLayout {
                spacing: 1
                Layout.alignment: Qt.AlignVCenter

                RowLayout {
                    spacing: 4
                    Text {
                        text: "MACTRA"
                        color: Theme.textPrimary
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                    Text {
                        text: "NVR"
                        color: Theme.accentPrimary
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                }

                Text {
                    text: "ENTERPRISE SURVEILLANCE"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.4
                    font.family: Theme.fontSans
                }
            }
        }

        Item { Layout.fillWidth: true }

        // Telemetry Chips (Matching Web Console)
        RowLayout {
            spacing: 10
            Layout.alignment: Qt.AlignVCenter

            // Core Status Chip
            Rectangle {
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 8
                Layout.preferredHeight: 32
                Layout.preferredWidth: coreRow.implicitWidth + 20

                RowLayout {
                    id: coreRow
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "shield"
                        size: 14
                        color: Theme.statusOnline
                        strokeWidth: 2
                    }

                    Text {
                        text: "Core"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }

                    Text {
                        text: root.coreStatus
                        color: Theme.statusOnline
                        font.bold: true
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }
                }
            }

            // CPU Chip
            Rectangle {
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 8
                Layout.preferredHeight: 32
                Layout.preferredWidth: cpuRow.implicitWidth + 20

                RowLayout {
                    id: cpuRow
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "cpu"
                        size: 14
                        color: Theme.textMuted
                        strokeWidth: 2
                    }

                    Text {
                        text: "CPU"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }

                    Text {
                        text: root.cpuLoad
                        color: Theme.textPrimary
                        font.bold: true
                        font.pixelSize: 11
                        font.family: Theme.fontMono
                    }
                }
            }

            // RAM Free Chip
            Rectangle {
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 8
                Layout.preferredHeight: 32
                Layout.preferredWidth: ramRow.implicitWidth + 20

                RowLayout {
                    id: ramRow
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "ram"
                        size: 14
                        color: Theme.textMuted
                        strokeWidth: 2
                    }

                    Text {
                        text: "RAM free"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }

                    Text {
                        text: root.ramFree
                        color: Theme.textPrimary
                        font.bold: true
                        font.pixelSize: 11
                        font.family: Theme.fontMono
                    }
                }
            }

            // Storage Chip
            Rectangle {
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 8
                Layout.preferredHeight: 32
                Layout.preferredWidth: storageRow.implicitWidth + 20

                RowLayout {
                    id: storageRow
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "storage"
                        size: 14
                        color: Theme.textMuted
                        strokeWidth: 2
                    }

                    Text {
                        text: "Storage"
                        color: Theme.textMuted
                        font.pixelSize: 11
                        font.family: Theme.fontSans
                    }

                    Text {
                        text: root.storageFree
                        color: Theme.textPrimary
                        font.bold: true
                        font.pixelSize: 11
                        font.family: Theme.fontMono
                    }
                }
            }

            // Live Surveillance Clock Pill
            Rectangle {
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 8
                Layout.preferredHeight: 32
                Layout.preferredWidth: clockRow.implicitWidth + 20

                RowLayout {
                    id: clockRow
                    anchors.centerIn: parent
                    spacing: 8

                    AppIcon {
                        name: "lightning"
                        size: 14
                        color: Theme.accentPrimary
                        strokeWidth: 2
                    }

                    Text {
                        id: clockLabel
                        text: "----‐--‐-- --:--:--"
                        color: Theme.textPrimary
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontMono
                    }
                }
            }
        }
    }
}

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "."
import "components"
import "pages"

ApplicationWindow {
    id: appWindow
    visible: true
    width: 1366
    height: 820
    minimumWidth: 1024
    minimumHeight: 680
    title: "MACTRA NVR - Enterprise Surveillance System"
    color: Theme.bgApp

    property string currentRoute: "live"
    property alias liveLayoutMode: livePageView.layoutMode
    property string toastText: ""
    property bool toastVisible: false

    function showToast(msg) {
        appWindow.toastText = msg;
        appWindow.toastVisible = true;
        toastTimer.restart();
    }

    Timer {
        id: toastTimer
        interval: 3500
        repeat: false
        onTriggered: appWindow.toastVisible = false
    }

    // Main Application Console
    Item {
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Top Header Bar
            AppHeader {
                id: mainHeader
                Layout.fillWidth: true
            }

            // Main Console Workspace (Sidebar + Content View)
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Left Navigation Rail
                AppSidebar {
                    id: mainSidebar
                    Layout.fillHeight: true
                    activeRoute: appWindow.currentRoute
                    onRouteSelected: function(route) {
                        appWindow.currentRoute = route;
                    }
                }

                // Center Content View Area
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.bgApp
                    clip: true

                    // StackLayout switching between pages
                    StackLayout {
                        id: pageStack
                        anchors.fill: parent
                        currentIndex: {
                            if (appWindow.currentRoute === "live") return 0;
                            if (appWindow.currentRoute === "playback") return 1;
                            if (appWindow.currentRoute === "cameras") return 2;
                            if (appWindow.currentRoute === "dashboard") return 3;
                            if (appWindow.currentRoute === "system") return 4;
                            return 0;
                        }

                        // Index 0: Live Matrix Flagship View
                        LivePage {
                            id: livePageView
                        }

                        // Index 1: Recorded Footage & 24h Interactive Scrub Bar
                        PlaybackPage {
                            id: playbackPageView
                        }

                        // Index 2: Ingress Camera Management & ONVIF Discovery
                        CamerasPage {
                            id: camerasPageView
                        }

                        // Index 3: Telemetry, Hardware & Channel Health Dashboard
                        DashboardPage {
                            id: dashboardPageView
                            onNavigateTo: function(route) {
                                appWindow.currentRoute = route;
                                mainSidebar.activeRoute = route;
                            }
                        }

                        // Index 4: Diagnostics, Storage Allocation & User Access Control
                        SystemPage {
                            id: systemPageView
                        }
                    }
                }
            }
        }
    }

    // 3. Floating Enterprise Toast Notification Banner (Bottom-Right matching Web)
    Rectangle {
        id: toastBanner
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        anchors.right: parent.right
        anchors.rightMargin: 20
        width: Math.max(280, toastRow.implicitWidth + 32)
        height: 42
        radius: 12
        color: "#ffffff"
        border.color: "#bfdbfe"
        border.width: 1
        visible: appWindow.toastVisible
        opacity: appWindow.toastVisible ? 1.0 : 0.0
        z: 10000

        RowLayout {
            id: toastRow
            anchors.centerIn: parent
            spacing: 10

            AppIcon {
                name: "info"
                size: 15
                color: "#1e40af"
                strokeWidth: 2
            }

            Text {
                id: toastLabel
                text: appWindow.toastText
                color: "#1e40af"
                font.pixelSize: 12
                font.weight: Font.Medium
                font.family: Theme.fontSans
            }
        }

        Behavior on opacity {
            NumberAnimation { duration: 180 }
        }
    }
}

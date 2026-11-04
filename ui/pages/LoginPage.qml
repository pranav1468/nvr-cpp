import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

/**
 * Enterprise Full-Screen Login Page
 * Pixel-exact parity with Web Console (#page-login)
 */
Rectangle {
    id: root
    anchors.fill: parent
    color: Theme.bgApp

    property bool loading: false
    property string errorMessage: ""
    property bool showPassword: false

    function doLogin() {
        var u = usernameInput.text.trim();
        var p = passwordInput.text;
        if (!u || !p) {
            root.errorMessage = "Please enter both username and password.";
            return;
        }

        root.loading = true;
        root.errorMessage = "";

        ApiClient.login(u, p, function(res) {
            root.loading = false;
            passwordInput.text = "";
        }, function(err) {
            root.loading = false;
            root.errorMessage = err || "Invalid username or password.";
        });
    }

    // Centered Login Card
    Rectangle {
        id: card
        width: Math.min(parent.width - 32, 420)
        implicitHeight: cardCol.implicitHeight + 64
        anchors.centerIn: parent
        radius: 16
        color: Theme.bgCard
        border.color: Theme.borderSubtle
        border.width: 1

        ColumnLayout {
            id: cardCol
            anchors.fill: parent
            anchors.margins: 32
            spacing: 20

            // Brand Header
            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredHeight: 48
                    Layout.preferredWidth: 48
                    radius: 12
                    color: Theme.accentPrimary

                    AppIcon {
                        anchors.centerIn: parent
                        name: "video"
                        size: 24
                        color: "#ffffff"
                        strokeWidth: 2
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 4
                    Text {
                        text: "MACTRA"
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                    Text {
                        text: "NVR"
                        color: Theme.accentPrimary
                        font.pixelSize: 20
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "ENTERPRISE SURVEILLANCE"
                    color: Theme.textMuted
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 1.6
                    font.family: Theme.fontSans
                }
            }

            // Title & Subtitle
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "Sign In to NVR"
                    color: Theme.textPrimary
                    font.pixelSize: 17
                    font.bold: true
                    font.family: Theme.fontSans
                    Layout.alignment: Qt.AlignHCenter
                }

                Text {
                    text: "Enter your credentials to access live matrix and control functions."
                    color: Theme.textMuted
                    font.pixelSize: 12
                    font.family: Theme.fontSans
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }

            // Error Banner
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: errorRow.implicitHeight + 16
                radius: 8
                color: "#fef2f2"
                border.color: "#fecaca"
                border.width: 1
                visible: root.errorMessage.length > 0

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8

                    Text {
                        text: "⚠"
                        color: Theme.statusDanger
                        font.pixelSize: 14
                        font.bold: true
                    }

                    Text {
                        text: root.errorMessage
                        color: Theme.statusDanger
                        font.pixelSize: 12
                        font.family: Theme.fontSans
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // Form Inputs
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 14

                // Username
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "Username"
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.family: Theme.fontSans
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        radius: 8
                        color: Theme.bgSurface
                        border.color: usernameInput.activeFocus ? Theme.accentPrimary : Theme.borderLight
                        border.width: usernameInput.activeFocus ? 2 : 1

                        TextField {
                            id: usernameInput
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            background: null
                            color: Theme.textPrimary
                            placeholderText: "admin"
                            placeholderTextColor: Theme.textDim
                            font.pixelSize: 13
                            font.family: Theme.fontSans
                            selectByMouse: true
                            onAccepted: passwordInput.forceActiveFocus()
                        }
                    }
                }

                // Password
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: "Password"
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.family: Theme.fontSans
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        radius: 8
                        color: Theme.bgSurface
                        border.color: passwordInput.activeFocus ? Theme.accentPrimary : Theme.borderLight
                        border.width: passwordInput.activeFocus ? 2 : 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 0

                            TextField {
                                id: passwordInput
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                background: null
                                color: Theme.textPrimary
                                placeholderText: "••••••••"
                                placeholderTextColor: Theme.textDim
                                echoMode: root.showPassword ? TextInput.Normal : TextInput.Password
                                font.pixelSize: 13
                                font.family: Theme.fontSans
                                selectByMouse: true
                                onAccepted: root.doLogin()
                            }

                            // Eye Toggle
                            Rectangle {
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                radius: 6
                                color: eyeMouse.containsMouse ? Theme.bgSecondary : "transparent"

                                AppIcon {
                                    anchors.centerIn: parent
                                    name: root.showPassword ? "eye_off" : "eye"
                                    size: 16
                                    color: Theme.textMuted
                                    strokeWidth: 1.8
                                }

                                MouseArea {
                                    id: eyeMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.showPassword = !root.showPassword
                                }
                            }
                        }
                    }
                }
            }

            // Sign In Button
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 8
                color: loginMouse.pressed ? Theme.accentBlueHover : (loginMouse.containsMouse ? Theme.accentBlueHover : Theme.accentPrimary)
                opacity: root.loading ? 0.7 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: root.loading ? "Signing In..." : "Sign In"
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.bold: true
                    font.family: Theme.fontSans
                }

                MouseArea {
                    id: loginMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    enabled: !root.loading
                    onClicked: root.doLogin()
                }
            }

            // Footer Credentials Note
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.borderSubtle
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 4

                Text {
                    text: "Default credentials: "
                    color: Theme.textMuted
                    font.pixelSize: 11
                    font.family: Theme.fontSans
                }
                Text {
                    text: "admin"
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.bold: true
                    font.family: Theme.fontMono
                }
                Text {
                    text: " / "
                    color: Theme.textMuted
                    font.pixelSize: 11
                }
                Text {
                    text: "Admin@123"
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.bold: true
                    font.family: Theme.fontMono
                }
            }
        }
    }
}

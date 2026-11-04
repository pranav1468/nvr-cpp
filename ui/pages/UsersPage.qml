import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

/**
 * Enterprise User Management & Access Control (RBAC) Page
 * Full visual and functional parity with Web Console (#view-users)
 */
Rectangle {
    id: root
    color: Theme.bgApp

    property var usersList: []
    property int totalCount: 0
    property int adminCount: 0
    property int operatorCount: 0
    property int viewerCount: 0
    property string activeResetUser: ""

    Component.onCompleted: {
        loadUsers();
    }

    function loadUsers() {
        ApiClient.getUsers(function(list) {
            root.usersList = list || [];
            var tot = 0, adm = 0, op = 0, vw = 0;
            userModel.clear();
            for (var i = 0; i < root.usersList.length; ++i) {
                var u = root.usersList[i];
                userModel.append({
                    username: u.username || "",
                    role: u.role || "OPERATOR",
                    created_at: u.created_at || "System",
                    status: "ACTIVE"
                });
                tot++;
                var r = (u.role || "").toUpperCase();
                if (r === "ADMIN") adm++;
                else if (r === "OPERATOR") op++;
                else if (r === "VIEWER") vw++;
            }
            root.totalCount = tot;
            root.adminCount = adm;
            root.operatorCount = op;
            root.viewerCount = vw;
        }, function(err) {
            console.warn("Failed to load users:", err);
        });
    }

    ListModel {
        id: userModel
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: contentCol.implicitHeight + 60
        clip: true

        ColumnLayout {
            id: contentCol
            width: Math.min(parent.width - 48, 1200)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 24
            spacing: 20

            // 1. Top Header Bar
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    Text {
                        text: "User Management & Access Control"
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        font.family: Theme.fontDisplay
                    }

                    Text {
                        text: "Local operator accounts · role privileges · PBKDF2 authentication"
                        color: Theme.textMuted
                        font.pixelSize: 13
                        font.family: Theme.fontSans
                    }
                }

                // Refresh Button
                Rectangle {
                    Layout.preferredHeight: 34
                    Layout.preferredWidth: refreshRow.implicitWidth + 24
                    radius: 8
                    color: refreshMouse.containsMouse ? Theme.bgSecondary : Theme.bgSurface
                    border.color: Theme.borderSubtle
                    border.width: 1

                    RowLayout {
                        id: refreshRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "refresh"
                            size: 13
                            color: Theme.textSecondary
                            strokeWidth: 2
                        }

                        Text {
                            text: "Refresh"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        id: refreshMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.loadUsers()
                    }
                }

                // Add User Button
                Rectangle {
                    Layout.preferredHeight: 34
                    Layout.preferredWidth: addRow.implicitWidth + 24
                    radius: 8
                    color: addMouse.containsMouse ? Theme.accentBlueHover : Theme.accentPrimary

                    RowLayout {
                        id: addRow
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            name: "plus"
                            size: 14
                            color: "#ffffff"
                            strokeWidth: 2
                        }

                        Text {
                            text: "Add User"
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontSans
                        }
                    }

                    MouseArea {
                        id: addMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: addUserModal.open()
                    }
                }
            }

            // 2. 4 KPI Metric Summary Cards
            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                // Total Accounts
                Rectangle {
                    Layout.fillWidth: true
                    height: 94
                    radius: 12
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Total Accounts"
                                color: Theme.textMuted
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                width: 22; height: 22; radius: 6; color: Theme.bgBadgeBlue
                                AppIcon { anchors.centerIn: parent; name: "users"; size: 12; color: Theme.accentPrimary; strokeWidth: 2 }
                            }
                        }

                        Text {
                            text: String(root.totalCount)
                            color: Theme.textPrimary
                            font.pixelSize: 24
                            font.bold: true
                            font.family: Theme.fontMono
                        }
                    }
                }

                // Administrators
                Rectangle {
                    Layout.fillWidth: true
                    height: 94
                    radius: 12
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Administrators"
                                color: Theme.textMuted
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                width: 22; height: 22; radius: 6; color: "#f3e8ff"
                                AppIcon { anchors.centerIn: parent; name: "shield"; size: 12; color: "#7c3aed"; strokeWidth: 2 }
                            }
                        }

                        Text {
                            text: String(root.adminCount)
                            color: "#7c3aed"
                            font.pixelSize: 24
                            font.bold: true
                            font.family: Theme.fontMono
                        }
                    }
                }

                // Operators
                Rectangle {
                    Layout.fillWidth: true
                    height: 94
                    radius: 12
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Operators"
                                color: Theme.textMuted
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                width: 22; height: 22; radius: 6; color: "#dcfce7"
                                AppIcon { anchors.centerIn: parent; name: "video"; size: 12; color: "#16a34a"; strokeWidth: 2 }
                            }
                        }

                        Text {
                            text: String(root.operatorCount)
                            color: "#16a34a"
                            font.pixelSize: 24
                            font.bold: true
                            font.family: Theme.fontMono
                        }
                    }
                }

                // Viewers
                Rectangle {
                    Layout.fillWidth: true
                    height: 94
                    radius: 12
                    color: Theme.bgCard
                    border.color: Theme.borderSubtle
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Viewers"
                                color: Theme.textMuted
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                width: 22; height: 22; radius: 6; color: "#f1f5f9"
                                AppIcon { anchors.centerIn: parent; name: "live"; size: 12; color: "#64748b"; strokeWidth: 2 }
                            }
                        }

                        Text {
                            text: String(root.viewerCount)
                            color: "#64748b"
                            font.pixelSize: 24
                            font.bold: true
                            font.family: Theme.fontMono
                        }
                    }
                }
            }

            // 3. User Accounts Table Card
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: tableCol.implicitHeight + 32
                radius: 14
                color: Theme.bgCard
                border.color: Theme.borderSubtle
                border.width: 1

                ColumnLayout {
                    id: tableCol
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 0

                    // Table Header
                    Rectangle {
                        Layout.fillWidth: true
                        height: 38
                        color: Theme.bgSecondary
                        radius: 8

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 12

                            Text {
                                text: "USER"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.5
                                Layout.preferredWidth: 200
                            }
                            Text {
                                text: "ROLE"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.5
                                Layout.preferredWidth: 120
                            }
                            Text {
                                text: "STATUS"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.5
                                Layout.preferredWidth: 110
                            }
                            Text {
                                text: "ENCRYPTION POLICY"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.5
                                Layout.fillWidth: true
                            }
                            Text {
                                text: "ACTIONS"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                font.bold: true
                                font.letterSpacing: 0.5
                                horizontalAlignment: Text.AlignRight
                                Layout.preferredWidth: 160
                            }
                        }
                    }

                    // Table Rows
                    ListView {
                        id: usersView
                        Layout.fillWidth: true
                        implicitHeight: contentHeight
                        interactive: false
                        model: userModel
                        spacing: 0

                        delegate: Rectangle {
                            id: rowRect
                            width: usersView.width
                            height: 52
                            color: rowMouse.containsMouse ? Theme.bgCardHover : "transparent"
                            border.color: Theme.borderSubtle
                            border.width: 0

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.borderSubtle
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                spacing: 12

                                // User Avatar + Username
                                RowLayout {
                                    Layout.preferredWidth: 200
                                    spacing: 10

                                    Rectangle {
                                        width: 30
                                        height: 30
                                        radius: 15
                                        color: model.role === "ADMIN" ? "#f3e8ff" : (model.role === "OPERATOR" ? "#dcfce7" : "#f1f5f9")

                                        Text {
                                            anchors.centerIn: parent
                                            text: model.username.charAt(0).toUpperCase()
                                            color: model.role === "ADMIN" ? "#7c3aed" : (model.role === "OPERATOR" ? "#16a34a" : "#64748b")
                                            font.pixelSize: 12
                                            font.bold: true
                                        }
                                    }

                                    Text {
                                        text: model.username
                                        color: Theme.textPrimary
                                        font.pixelSize: 13
                                        font.bold: true
                                        font.family: Theme.fontMono
                                    }
                                }

                                // Role Badge
                                Item {
                                    Layout.preferredWidth: 120
                                    Layout.fillHeight: true

                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        height: 22
                                        width: roleBadgeText.implicitWidth + 14
                                        radius: 6
                                        color: model.role === "ADMIN" ? "#f3e8ff" : (model.role === "OPERATOR" ? "#dcfce7" : "#f1f5f9")

                                        Text {
                                            id: roleBadgeText
                                            anchors.centerIn: parent
                                            text: model.role
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: model.role === "ADMIN" ? "#7c3aed" : (model.role === "OPERATOR" ? "#16a34a" : "#64748b")
                                        }
                                    }
                                }

                                // Status Pill
                                Item {
                                    Layout.preferredWidth: 110
                                    Layout.fillHeight: true

                                    RowLayout {
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 6

                                        Rectangle {
                                            width: 7
                                            height: 7
                                            radius: 3.5
                                            color: Theme.statusOnline
                                        }

                                        Text {
                                            text: "Active"
                                            color: Theme.textPrimary
                                            font.pixelSize: 12
                                            font.family: Theme.fontSans
                                        }
                                    }
                                }

                                // Policy
                                Text {
                                    text: "PBKDF2-HMAC-SHA256 (100k)"
                                    color: Theme.textMuted
                                    font.pixelSize: 12
                                    font.family: Theme.fontMono
                                    Layout.fillWidth: true
                                }

                                // Action Buttons
                                RowLayout {
                                    Layout.preferredWidth: 160
                                    spacing: 8
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                                    // Reset Password
                                    Rectangle {
                                        height: 28
                                        width: resetRow.implicitWidth + 16
                                        radius: 6
                                        color: resetMouse.containsMouse ? Theme.bgSecondary : "transparent"
                                        border.color: Theme.borderSubtle
                                        border.width: 1

                                        RowLayout {
                                            id: resetRow
                                            anchors.centerIn: parent
                                            spacing: 4

                                            AppIcon {
                                                name: "key"
                                                size: 11
                                                color: Theme.textSecondary
                                                strokeWidth: 2
                                            }

                                            Text {
                                                text: "Reset"
                                                color: Theme.textSecondary
                                                font.pixelSize: 11
                                                font.weight: Font.DemiBold
                                                font.family: Theme.fontSans
                                            }
                                        }

                                        MouseArea {
                                            id: resetMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.activeResetUser = model.username;
                                                resetPwdModal.targetUser = model.username;
                                                resetPwdModal.open();
                                            }
                                        }
                                    }

                                    // Delete User
                                    Rectangle {
                                        height: 28
                                        width: 28
                                        radius: 6
                                        color: delMouse.containsMouse ? "#fee2e2" : "transparent"
                                        border.color: Theme.borderSubtle
                                        border.width: 1
                                        visible: model.username !== ApiClient.currentUsername && model.username !== "admin"

                                        AppIcon {
                                            anchors.centerIn: parent
                                            name: "trash"
                                            size: 12
                                            color: delMouse.containsMouse ? Theme.statusDanger : Theme.textMuted
                                            strokeWidth: 2
                                        }

                                        MouseArea {
                                            id: delMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                ApiClient.deleteUser(model.username, function() {
                                                    root.loadUsers();
                                                }, function(err) {
                                                    console.warn("Delete user failed:", err);
                                                });
                                            }
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                propagateComposedEvents: true
                            }
                        }
                    }
                }
            }
        }
    }

    // ==========================================
    // Add User Modal Dialog
    // ==========================================
    Dialog {
        id: addUserModal
        anchors.centerIn: parent
        width: 380
        modal: true
        closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
        background: Rectangle {
            color: Theme.bgModal
            border.color: Theme.borderSubtle
            border.width: 1
            radius: 16
        }

        property string errorMsg: ""

        ColumnLayout {
            anchors.fill: parent
            spacing: 16

            Text {
                text: "Add New User"
                font.pixelSize: 17
                font.bold: true
                color: Theme.textPrimary
            }

            Text {
                text: "Create an operator or viewer account with secure PBKDF2 hashing."
                font.pixelSize: 12
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: addErrText.implicitHeight + 14
                radius: 6
                color: "#fef2f2"
                border.color: "#fecaca"
                border.width: 1
                visible: addUserModal.errorMsg.length > 0

                Text {
                    id: addErrText
                    anchors.centerIn: parent
                    text: addUserModal.errorMsg
                    color: Theme.statusDanger
                    font.pixelSize: 11
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { text: "Username"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.textPrimary }
                Rectangle {
                    Layout.fillWidth: true; height: 36; radius: 8; color: Theme.bgSurface; border.color: Theme.borderLight
                    TextField {
                        id: newUserName
                        anchors.fill: parent; anchors.margins: 8; background: null
                        placeholderText: "e.g. guard1"; font.pixelSize: 12; color: Theme.textPrimary
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { text: "Password"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.textPrimary }
                Rectangle {
                    Layout.fillWidth: true; height: 36; radius: 8; color: Theme.bgSurface; border.color: Theme.borderLight
                    TextField {
                        id: newUserPwd
                        anchors.fill: parent; anchors.margins: 8; background: null
                        echoMode: TextInput.Password
                        placeholderText: "Minimum 4 characters"; font.pixelSize: 12; color: Theme.textPrimary
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { text: "Role Privilege"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.textPrimary }
                ComboBox {
                    id: newUserRole
                    Layout.fillWidth: true
                    model: ["OPERATOR", "VIEWER", "ADMIN"]
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 10

                Button {
                    text: "Cancel"
                    Layout.fillWidth: true
                    onClicked: addUserModal.close()
                }

                Button {
                    text: "Create User"
                    Layout.fillWidth: true
                    highlighted: true
                    onClicked: {
                        var u = newUserName.text.trim();
                        var p = newUserPwd.text;
                        var r = newUserRole.currentText;
                        if (!u || !p) {
                            addUserModal.errorMsg = "Please fill in all fields.";
                            return;
                        }
                        ApiClient.addUser({ username: u, password: p, role: r }, function() {
                            addUserModal.close();
                            newUserName.text = "";
                            newUserPwd.text = "";
                            addUserModal.errorMsg = "";
                            root.loadUsers();
                        }, function(err) {
                            addUserModal.errorMsg = err || "Failed to create user.";
                        });
                    }
                }
            }
        }
    }

    // ==========================================
    // Reset Password Modal Dialog
    // ==========================================
    Dialog {
        id: resetPwdModal
        anchors.centerIn: parent
        width: 380
        modal: true
        closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
        property string targetUser: ""
        property string errorMsg: ""

        background: Rectangle {
            color: Theme.bgModal
            border.color: Theme.borderSubtle
            border.width: 1
            radius: 16
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 16

            Text {
                text: "Reset User Password"
                font.pixelSize: 17
                font.bold: true
                color: Theme.textPrimary
            }

            Text {
                text: "Set a new password for account: " + resetPwdModal.targetUser
                font.pixelSize: 12
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: resetErrText.implicitHeight + 14
                radius: 6
                color: "#fef2f2"
                border.color: "#fecaca"
                border.width: 1
                visible: resetPwdModal.errorMsg.length > 0

                Text {
                    id: resetErrText
                    anchors.centerIn: parent
                    text: resetPwdModal.errorMsg
                    color: Theme.statusDanger
                    font.pixelSize: 11
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { text: "New Password"; font.pixelSize: 12; font.weight: Font.DemiBold; color: Theme.textPrimary }
                Rectangle {
                    Layout.fillWidth: true; height: 36; radius: 8; color: Theme.bgSurface; border.color: Theme.borderLight
                    TextField {
                        id: resetPwdInput
                        anchors.fill: parent; anchors.margins: 8; background: null
                        echoMode: TextInput.Password
                        placeholderText: "••••••••"; font.pixelSize: 12; color: Theme.textPrimary
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 10

                Button {
                    text: "Cancel"
                    Layout.fillWidth: true
                    onClicked: resetPwdModal.close()
                }

                Button {
                    text: "Reset Password"
                    Layout.fillWidth: true
                    highlighted: true
                    onClicked: {
                        var p = resetPwdInput.text;
                        if (!p || p.length < 4) {
                            resetPwdModal.errorMsg = "Password must be at least 4 characters.";
                            return;
                        }
                        ApiClient.resetUserPassword({ username: resetPwdModal.targetUser, new_password: p }, function() {
                            resetPwdModal.close();
                            resetPwdInput.text = "";
                            resetPwdModal.errorMsg = "";
                        }, function(err) {
                            resetPwdModal.errorMsg = err || "Failed to reset password.";
                        });
                    }
                }
            }
        }
    }
}


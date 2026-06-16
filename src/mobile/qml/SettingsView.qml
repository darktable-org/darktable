import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Page {
    objectName: "settings"
    background: Rectangle { color: "#111111" }

    header: ToolBar {
        Material.background: "#1e1e1e"
        Label {
            anchors.centerIn: parent
            text: "Settings"
            font.pixelSize: 16
            font.bold: true
        }
    }

    ScrollView {
        anchors.fill: parent

        Column {
            width: parent.parent.width
            spacing: 0

            // ── section: connection ───────────────────────────────────────────
            SectionHeader { text: "P2P Connection" }

            SettingsRow {
                label: "Status"
                content: Row {
                    spacing: 8
                    Rectangle {
                        width: 10; height: 10; radius: 5
                        anchors.verticalCenter: parent.verticalCenter
                        color: daemon.running ? (p2p.connected ? "#20e020" : "#e0a020") : "#e02020"
                    }
                    Label {
                        text: daemon.status
                        color: "#ccc"
                        font.pixelSize: 13
                    }
                }
            }

            // Per-peer connection status as reported by daemon stderr.
            SettingsRow {
                label: "Peers"
                content: Column {
                    spacing: 6
                    topPadding: 4; bottomPadding: 4

                    Repeater {
                        model: Object.keys(daemon.peerStatuses)

                        delegate: Row {
                            required property string modelData
                            spacing: 8

                            Rectangle {
                                width: 8; height: 8; radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: {
                                    const s = daemon.peerStatuses[modelData]
                                    if (s === "ok")       return "#20e020"
                                    if (s === "refused")  return "#e02020"
                                    if (s === "timeout")  return "#e0a020"
                                    if (s === "no-route") return "#606060"
                                    return "#444"
                                }
                            }
                            Label {
                                text: {
                                    const s = daemon.peerStatuses[modelData]
                                    const tag = s === "ok" ? " ✓"
                                              : s === "refused"  ? " ✗ refused"
                                              : s === "timeout"  ? " ✗ timeout"
                                              : s === "no-route" ? " ✗ no route"
                                              : ""
                                    return modelData + tag
                                }
                                color: daemon.peerStatuses[modelData] === "ok" ? "#20e020" : "#888"
                                font.pixelSize: 11
                                wrapMode: Text.WrapAnywhere
                                width: 260
                            }
                        }
                    }

                    Label {
                        visible: Object.keys(daemon.peerStatuses).length === 0
                        text: daemon.staticPeers.length > 0
                              ? "Waiting for first sync cycle…"
                              : "No static peers (mDNS auto-discovery active)"
                        color: "#666"
                        font.pixelSize: 12
                    }
                }
            }

            // ── section: passphrase ───────────────────────────────────────────
            SectionHeader { text: "Shared Passphrase" }

            SettingsRow {
                label: "Passphrase"
                content: ColumnLayout {
                    spacing: 6
                    TextField {
                        id: passphraseField
                        Layout.preferredWidth: 240
                        text:         daemon.passphrase
                        echoMode:     revealToggle.checked
                                      ? TextInput.Normal : TextInput.Password
                        placeholderText: "Enter shared passphrase…"
                        font.pixelSize: 13
                        Material.accent: Material.Orange
                        onEditingFinished: daemon.setPassphrase(text)
                    }
                    CheckBox {
                        id: revealToggle
                        text: "Show"
                        font.pixelSize: 11
                        checked: false
                    }
                }
            }

            Label {
                width: parent.width
                leftPadding: 16; rightPadding: 16; topPadding: 4; bottomPadding: 12
                text: "All darktable instances must use the same passphrase. "
                    + "The passphrase derives the Ed25519 identity and TLS certificate; "
                    + "never share it over an insecure channel."
                color: "#666"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // ── section: static peers ─────────────────────────────────────────
            SectionHeader { text: "Static Peers" }

            SettingsRow {
                label: "Add peer"
                content: RowLayout {
                    spacing: 6
                    TextField {
                        id: peerField
                        Layout.preferredWidth: 200
                        placeholderText: "192.168.1.108 or https://…"
                        font.pixelSize: 13
                        Material.accent: Material.Orange
                    }
                    Button {
                        text: "Add"
                        highlighted: true
                        onClicked: {
                            const url = peerField.text.trim()
                            if (url) {
                                const full = url.startsWith("https://") ? url
                                           : "https://" + url + ":17842"
                                const current = daemon.staticPeers.slice()
                                current.push(full)
                                daemon.setStaticPeers(current)
                                peerField.text = ""
                            }
                        }
                    }
                }
            }

            Label {
                width: parent.width
                leftPadding: 16; rightPadding: 16; bottomPadding: 4
                text: "On the same LAN, peers are discovered automatically via mDNS. "
                    + "Add static peers for cross-network connections. "
                    + "Port 17842 must be reachable (UPnP/NAT-PMP is attempted automatically)."
                color: "#666"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // ── section: daemon control ───────────────────────────────────────
            SectionHeader { text: "Daemon" }

            Item {
                width: parent.width
                height: 60
                RowLayout {
                    anchors { fill: parent; margins: 16 }
                    spacing: 12

                    Button {
                        text: daemon.running ? "Restart" : "Start"
                        highlighted: !daemon.running
                        onClicked: daemon.running ? daemon.restart() : daemon.start()
                    }
                    Button {
                        text: "Stop"
                        visible: daemon.running
                        onClicked: daemon.stop()
                    }
                    Button {
                        text: "P2P Log"
                        flat: true
                        onClicked: logDrawer.open()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "Changes to the passphrase take effect after a restart."
                        color: "#666"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Item { height: 40; width: 1 }  // bottom padding
        }
    }

    // ── P2P log drawer ────────────────────────────────────────────────────────
    Drawer {
        id: logDrawer
        width: parent.width
        height: parent.height * 0.72
        edge: Qt.BottomEdge
        Material.theme: Material.Dark

        background: Rectangle { color: "#181818" }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 0
            spacing: 0

            // Header bar
            Rectangle {
                Layout.fillWidth: true
                height: 48
                color: "#1e1e1e"
                RowLayout {
                    anchors { fill: parent; leftMargin: 16; rightMargin: 8 }
                    Label {
                        Layout.fillWidth: true
                        text: "P2P Activity Log"
                        font { pixelSize: 14; bold: true }
                        color: "#fff"
                    }
                    ToolButton {
                        text: "✕"
                        font.pixelSize: 16
                        onClicked: logDrawer.close()
                    }
                }
            }

            // Filter chips
            Row {
                id: logFilterButtons
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.topMargin: 6
                Layout.bottomMargin: 4
                spacing: 6

                property string active: "all"

                Repeater {
                    model: ["all", "peer", "xmp", "proxy", "import"]
                    delegate: Rectangle {
                        required property string modelData

                        width: chipLabel.implicitWidth + 20
                        height: 26
                        radius: 13
                        color: logFilterButtons.active === modelData
                               ? Material.accentColor : "#2a2a2a"

                        Label {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: parent.modelData
                            font.pixelSize: 11
                            color: logFilterButtons.active === parent.modelData ? "#000" : "#888"
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: logFilterButtons.active = parent.modelData
                        }
                    }
                }
            }

            // Log list
            ListView {
                id: logView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 0

                model: {
                    const filter = logFilterButtons.active
                    if (filter === "all") return daemon.logLines
                    return daemon.logLines.filter(l => l.includes("[" + filter + "]"))
                }

                onCountChanged: if (atYEnd || count <= 1) positionViewAtEnd()

                delegate: Rectangle {
                    required property string modelData
                    required property int index
                    width: logView.width
                    height: logText.implicitHeight + 8
                    color: index % 2 === 0 ? "#181818" : "#1a1a1a"

                    property color lineColor: {
                        if (modelData.includes("[peer]"))   return "#6af"
                        if (modelData.includes("[xmp]"))    return "#af8"
                        if (modelData.includes("[proxy]"))  return "#fa8"
                        if (modelData.includes("[import]")) return "#f8f"
                        if (modelData.includes("[https]"))  return "#888"
                        return "#999"
                    }

                    Label {
                        id: logText
                        anchors { left: parent.left; right: parent.right;
                                  leftMargin: 10; rightMargin: 10; verticalCenter: parent.verticalCenter }
                        text: parent.modelData
                        color: parent.lineColor
                        font { pixelSize: 10; family: "monospace" }
                        wrapMode: Text.WrapAnywhere
                    }
                }

                ScrollBar.vertical: ScrollBar {}
            }

            // Bottom bar with clear button
            Rectangle {
                Layout.fillWidth: true
                height: 44
                color: "#1e1e1e"
                Button {
                    anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    text: "Clear log"
                    flat: true
                    font.pixelSize: 12
                    onClicked: daemon.clearLog()
                }
                Label {
                    anchors.centerIn: parent
                    text: daemon.logLines.length + " lines"
                    color: "#555"
                    font.pixelSize: 11
                }
            }
        }
    }

    // ── inline components ─────────────────────────────────────────────────────
    component SectionHeader: Rectangle {
        required property string text
        width: parent.width
        height: 36
        color: "#1e1e1e"
        Label {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            text: parent.text
            color: Material.accentColor
            font { pixelSize: 11; weight: Font.Medium; capitalization: Font.AllUppercase }
        }
    }

    component SettingsRow: Item {
        required property string label
        property alias content: contentLoader.sourceComponent

        width: parent.width
        height: Math.max(52, contentLoader.height + 24)

        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 1; color: "#2a2a2a"
        }

        Label {
            anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            text: parent.label
            color: "#aaa"
            font.pixelSize: 13
            width: 90
        }

        Loader {
            id: contentLoader
            anchors { left: parent.left; leftMargin: 114;
                      right: parent.right; rightMargin: 16;
                      verticalCenter: parent.verticalCenter }
        }
    }
}

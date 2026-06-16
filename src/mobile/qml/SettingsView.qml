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

            SettingsRow {
                label: "Peers"
                content: Label {
                    text: p2p.peers.length > 0
                          ? p2p.peers.join("\n")
                          : "None discovered yet"
                    color: "#888"
                    font.pixelSize: 12
                    wrapMode: Text.WrapAnywhere
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
                            if (peerField.text.trim()) {
                                const current = p2p.peers.slice()
                                current.push(peerField.text.trim())
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

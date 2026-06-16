import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

ApplicationWindow {
    id: root
    visible: true
    width:  390
    height: 844
    title:  "Darktable Mobile"

    Material.theme:  Material.Dark
    Material.accent: Material.Orange

    // Handle darktable://pair?d=... deep links from the Android intent system.
    // The link text is delivered via the DaemonManager (which monitors the
    // intent via QtAndroid / Activity.onNewIntent) or via initial URL on start.
    Connections {
        target: daemon
        function onDeepLinkReceived(url) {
            stack.push(pairingPage)
            // QrScanner isn't needed — parse the URL directly.
            if(pairing.parseUrl(url))
                pairingConfirmLoader.active = true
        }
    }

    // ── pairing confirm loader (used for deep-link flow) ──────────────────────
    Loader {
        id: pairingConfirmLoader
        active: false
        sourceComponent: Dialog {
            id: dlinkDialog
            visible: true
            modal: true
            title: "Accept Pairing?"
            Material.theme: Material.Dark
            anchors.centerIn: Overlay.overlay

            contentItem: Column {
                spacing: 12
                topPadding: 8; bottomPadding: 8
                leftPadding: 16; rightPadding: 16
                Label {
                    text: "Apply the scanned passphrase and connect to:"
                    color: "#ccc"; font.pixelSize: 13; wrapMode: Text.WordWrap; width: 260
                }
                Label {
                    text: pairing.pendingPeers.length > 0
                          ? pairing.pendingPeers
                          : "(no static peers — mDNS will discover nearby desktops)"
                    color: Material.accentColor; font.pixelSize: 12; wrapMode: Text.WrapAnywhere; width: 260
                }
            }
            footer: DialogButtonBox {
                Button {
                    text: "Accept"; highlighted: true
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                }
                Button {
                    text: "Cancel"; flat: true
                    DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                }
            }
            onAccepted: { pairing.accept(); pairingConfirmLoader.active = false }
            onRejected: { pairing.reject(); pairingConfirmLoader.active = false }
        }
    }

    // ── navigation stack ──────────────────────────────────────────────────────
    StackView {
        id: stack
        anchors {
            top:    parent.top
            left:   parent.left
            right:  parent.right
            bottom: navBar.top
        }
        initialItem: galleryPage
    }

    Component { id: galleryPage;   ThumbnailGrid  {} }
    Component { id: settingsPage;  SettingsView   {} }
    Component { id: pairingPage;   PairingView    { onDone: stack.pop() } }

    // ── bottom navigation bar ─────────────────────────────────────────────────
    TabBar {
        id: navBar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        Material.background: "#1a1a1a"

        TabButton {
            text: "Gallery"
            icon.name: "image"
            onClicked: {
                while (stack.depth > 1) stack.pop()
                if (stack.currentItem?.objectName !== "gallery")
                    stack.replace(galleryPage)
            }
        }
        TabButton {
            text: "Pair"
            icon.name: "qr_code_scanner"
            onClicked: {
                while (stack.depth > 1) stack.pop()
                stack.push(pairingPage)
            }
        }
        TabButton {
            text: "Settings"
            icon.name: "settings"
            onClicked: {
                while (stack.depth > 1) stack.pop()
                if (stack.currentItem?.objectName !== "settings")
                    stack.replace(settingsPage)
            }
        }
    }

    // ── connection status toast ───────────────────────────────────────────────
    Rectangle {
        id: statusBanner
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: visible ? 28 : 0
        color:  p2p.connected ? "#1a7a1a" : "#7a1a1a"
        visible: true

        Behavior on color { ColorAnimation { duration: 400 } }

        Text {
            anchors.centerIn: parent
            text: {
                if (!p2p.connected) return daemon.status
                const s = daemon.peerStatuses
                const n = Object.keys(s).filter(k => s[k] === "ok").length
                return "Connected  •  " + n + " peer" + (n === 1 ? "" : "s")
            }
            color:    "white"
            font.pixelSize: 11
        }
    }
}

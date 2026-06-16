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
            text: p2p.connected
                  ? "Connected  •  " + p2p.peers.length + " peer" + (p2p.peers.length === 1 ? "" : "s")
                  : daemon.status
            color:    "white"
            font.pixelSize: 11
        }
    }
}

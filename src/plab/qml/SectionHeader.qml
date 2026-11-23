import QtQuick
import QtQuick.Layouts

Item {
    id: root
    property string title
    Layout.fillWidth: true
    implicitHeight: 36

    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        text: root.title.toUpperCase()
        color: Theme.textDim
        font.pixelSize: 12
        font.letterSpacing: 1.2
        font.bold: true
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.line
    }
}

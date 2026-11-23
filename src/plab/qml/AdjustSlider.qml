import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// one Lightroom-style slider: name, value, a track. double-click resets it
ColumnLayout {
    id: root
    property string label
    property real from: -100
    property real to: 100
    property real value: 0
    property real neutral: 0
    property real stepSize: 1
    property int decimals: 0
    property string unit: ""
    // the track's colors, left to right, for temperature and tint
    property var gradient: []
    signal moved(real value)

    Layout.fillWidth: true
    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Text {
            text: root.label
            color: Theme.text
            font.pixelSize: 14
            Layout.fillWidth: true
        }
        Text {
            text: (root.value > 0 && root.from < 0 ? "+" : "")
                  + root.value.toFixed(root.decimals) + root.unit
            color: Math.abs(root.value - root.neutral) > 1e-6 ? Theme.accent : Theme.textDim
            font.pixelSize: 14
            font.family: "monospace"
        }
    }

    Slider {
        id: slider
        Layout.fillWidth: true
        from: root.from
        to: root.to
        stepSize: root.stepSize
        value: root.value
        onMoved: root.moved(value)

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: 4
            radius: 2
            color: Theme.line
            gradient: root.gradient.length ? grad : null
            Gradient {
                id: grad
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: root.gradient.length ? root.gradient[0] : "transparent" }
                GradientStop { position: 1; color: root.gradient.length ? root.gradient[1] : "transparent" }
            }
        }

        TapHandler {
            onDoubleTapped: root.moved(root.neutral)
        }
    }
}

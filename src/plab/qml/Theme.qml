pragma Singleton
import QtQuick

// the friendly palette from the GTK custom build: slate panels, one accent
QtObject {
    readonly property color window: "#1e2227"
    readonly property color panel: "#272c33"
    readonly property color panelRaised: "#313741"
    readonly property color line: "#3a414c"
    readonly property color text: "#e6e9ee"
    readonly property color textDim: "#9aa3af"
    readonly property color accent: "#4da3ff"
    readonly property int gutter: 16
    readonly property int sliderHeight: 52
}

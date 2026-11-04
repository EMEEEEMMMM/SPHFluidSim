import QtQuick

MouseArea {
    id: handle

    required property var targetWindow
    required property int edges

    enabled: targetWindow.visibility !== Window.Maximized
             && targetWindow.visibility !== Window.FullScreen
    hoverEnabled: true
    preventStealing: true
    z: 100
    Accessible.ignored: true

    onPressed: function(mouse) {
        if (mouse.button !== Qt.LeftButton) {
            mouse.accepted = false;
            return;
        }
        mouse.accepted = true;
        targetWindow.startSystemResize(edges);
    }
}

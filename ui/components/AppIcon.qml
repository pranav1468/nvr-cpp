import QtQuick 2.15

/**
 * Universal Vector Icon Component
 * Renders exact Lucide / Heroicons vector paths using hardware-accelerated 2D canvas
 */
Item {
    id: root
    property string name: "video"
    property color color: "#64748b"
    property real size: 16
    property real strokeWidth: 1.75

    width: size
    height: size

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        renderTarget: Canvas.FramebufferObject

        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var w = width;
            var h = height;
            var s = w / 24.0; // Normalized to 24x24 viewBox

            ctx.save();
            ctx.scale(s, s);
            ctx.strokeStyle = root.color;
            ctx.fillStyle = root.color;
            ctx.lineWidth = root.strokeWidth;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";

            ctx.beginPath();

            if (root.name === "video" || root.name === "live") {
                // Video camera
                ctx.rect(3, 7, 12, 10);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(15, 10.5);
                ctx.lineTo(20, 7.5);
                ctx.lineTo(20, 16.5);
                ctx.lineTo(15, 13.5);
                ctx.closePath();
                ctx.stroke();
            } else if (root.name === "clock" || root.name === "recordings") {
                // Clock / History
                ctx.arc(12, 12, 9, 0, Math.PI * 2);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(12, 7);
                ctx.lineTo(12, 12);
                ctx.lineTo(15.5, 14);
                ctx.stroke();
            } else if (root.name === "camera" || root.name === "cameras") {
                // Photo camera
                ctx.moveTo(20, 8);
                ctx.lineTo(18, 8);
                ctx.lineTo(16.5, 5.5);
                ctx.lineTo(7.5, 5.5);
                ctx.lineTo(6, 8);
                ctx.lineTo(4, 8);
                ctx.arcTo(2, 8, 2, 10, 2);
                ctx.lineTo(2, 18);
                ctx.arcTo(2, 20, 4, 20, 2);
                ctx.lineTo(20, 20);
                ctx.arcTo(22, 20, 22, 18, 2);
                ctx.lineTo(22, 10);
                ctx.arcTo(22, 8, 20, 8, 2);
                ctx.closePath();
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(12, 14, 3.5, 0, Math.PI * 2);
                ctx.stroke();
            } else if (root.name === "analytics" || root.name === "dashboard") {
                // Bar charts
                ctx.rect(4, 13, 4, 7);
                ctx.rect(10, 9, 4, 11);
                ctx.rect(16, 5, 4, 15);
                ctx.stroke();
            } else if (root.name === "server" || root.name === "system") {
                // Server rack
                ctx.rect(3, 4, 18, 6);
                ctx.rect(3, 14, 18, 6);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(7, 7, 0.8, 0, Math.PI * 2);
                ctx.arc(7, 17, 0.8, 0, Math.PI * 2);
                ctx.fill();
            } else if (root.name === "shield" || root.name === "core") {
                // Shield check
                ctx.moveTo(12, 3);
                ctx.lineTo(19, 6);
                ctx.lineTo(19, 12);
                ctx.bezierCurveTo(19, 16.5, 16, 19.5, 12, 21);
                ctx.bezierCurveTo(8, 19.5, 5, 16.5, 5, 12);
                ctx.lineTo(5, 6);
                ctx.closePath();
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(9, 12);
                ctx.lineTo(11, 14);
                ctx.lineTo(15, 10);
                ctx.stroke();
            } else if (root.name === "cpu") {
                // CPU chip
                ctx.rect(5, 5, 14, 14);
                ctx.rect(9, 9, 6, 6);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(9, 1); ctx.lineTo(9, 5);
                ctx.moveTo(15, 1); ctx.lineTo(15, 5);
                ctx.moveTo(9, 19); ctx.lineTo(9, 23);
                ctx.moveTo(15, 19); ctx.lineTo(15, 23);
                ctx.moveTo(1, 9); ctx.lineTo(5, 9);
                ctx.moveTo(1, 15); ctx.lineTo(5, 15);
                ctx.moveTo(19, 9); ctx.lineTo(23, 9);
                ctx.moveTo(19, 15); ctx.lineTo(23, 15);
                ctx.stroke();
            } else if (root.name === "ram") {
                // RAM stick
                ctx.rect(3, 6, 18, 12);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(7, 10); ctx.lineTo(7, 14);
                ctx.moveTo(11, 10); ctx.lineTo(11, 14);
                ctx.moveTo(15, 10); ctx.lineTo(15, 14);
                ctx.stroke();
            } else if (root.name === "storage") {
                // Disk storage
                ctx.rect(3, 5, 18, 14);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(7, 12, 1, 0, Math.PI * 2);
                ctx.arc(11, 12, 1, 0, Math.PI * 2);
                ctx.fill();
            } else if (root.name === "lightning") {
                // Lightning bolt
                ctx.moveTo(13, 2);
                ctx.lineTo(4, 13);
                ctx.lineTo(11, 13);
                ctx.lineTo(11, 22);
                ctx.lineTo(20, 11);
                ctx.lineTo(13, 11);
                ctx.closePath();
                ctx.fill();
            } else if (root.name === "collapse") {
                // Chevron left collapse
                ctx.moveTo(15, 6);
                ctx.lineTo(9, 12);
                ctx.lineTo(15, 18);
                ctx.moveTo(19, 6);
                ctx.lineTo(13, 12);
                ctx.lineTo(19, 18);
                ctx.stroke();
            } else if (root.name === "expand" || root.name === "maximize") {
                // Maximize / Expand (Lucide maximize-2)
                ctx.moveTo(15, 3); ctx.lineTo(21, 3); ctx.lineTo(21, 9);
                ctx.moveTo(9, 21); ctx.lineTo(3, 21); ctx.lineTo(3, 15);
                ctx.moveTo(21, 3); ctx.lineTo(14, 10);
                ctx.moveTo(3, 21); ctx.lineTo(10, 14);
                ctx.stroke();
            } else if (root.name === "minimize" || root.name === "contract") {
                // Minimize / Contract (Lucide minimize-2)
                ctx.moveTo(4, 14); ctx.lineTo(10, 14); ctx.lineTo(10, 20);
                ctx.moveTo(20, 10); ctx.lineTo(14, 10); ctx.lineTo(14, 4);
                ctx.moveTo(14, 10); ctx.lineTo(21, 3);
                ctx.moveTo(3, 21); ctx.lineTo(10, 14);
                ctx.stroke();
            } else if (root.name === "search") {
                // Search magnifying glass
                ctx.arc(10.5, 10.5, 6.5, 0, Math.PI * 2);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(15.5, 15.5);
                ctx.lineTo(20.5, 20.5);
                ctx.stroke();
            } else if (root.name === "refresh") {
                // Refresh circle
                ctx.arc(12, 12, 7, 0.4, Math.PI * 1.7);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(19, 8);
                ctx.lineTo(19, 12);
                ctx.lineTo(15, 12);
                ctx.stroke();
            } else if (root.name === "plus") {
                // Plus sign
                ctx.moveTo(12, 5); ctx.lineTo(12, 19);
                ctx.moveTo(5, 12); ctx.lineTo(19, 12);
                ctx.stroke();
            } else if (root.name === "wifi") {
                // Wifi broadcast
                ctx.arc(12, 18, 1.5, 0, Math.PI * 2);
                ctx.fill();
                ctx.beginPath();
                ctx.arc(12, 18, 5, Math.PI * 1.25, Math.PI * 1.75);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(12, 18, 9, Math.PI * 1.25, Math.PI * 1.75);
                ctx.stroke();
            } else if (root.name === "cards") {
                // 4-box cards icon
                ctx.rect(3, 3, 7, 7);
                ctx.rect(14, 3, 7, 7);
                ctx.rect(14, 14, 7, 7);
                ctx.rect(3, 14, 7, 7);
                ctx.stroke();
            } else if (root.name === "play") {
                // Play triangle
                ctx.moveTo(6, 4);
                ctx.lineTo(19, 12);
                ctx.lineTo(6, 20);
                ctx.closePath();
                ctx.stroke();
            } else if (root.name === "list") {
                // List lines
                ctx.moveTo(8, 6); ctx.lineTo(20, 6);
                ctx.moveTo(8, 12); ctx.lineTo(20, 12);
                ctx.moveTo(8, 18); ctx.lineTo(20, 18);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(4, 6, 1, 0, Math.PI * 2);
                ctx.arc(4, 12, 1, 0, Math.PI * 2);
                ctx.arc(4, 18, 1, 0, Math.PI * 2);
                ctx.fill();
            } else if (root.name === "chevron_down") {
                // Chevron down
                ctx.moveTo(6, 9);
                ctx.lineTo(12, 15);
                ctx.lineTo(18, 9);
                ctx.stroke();
            } else if (root.name === "calendar") {
                // Calendar icon
                ctx.rect(3, 5, 18, 15);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(3, 10); ctx.lineTo(21, 10);
                ctx.moveTo(8, 2); ctx.lineTo(8, 6);
                ctx.moveTo(16, 2); ctx.lineTo(16, 6);
                ctx.stroke();
            } else if (root.name === "info") {
                // Info circle
                ctx.arc(12, 12, 8.5, 0, Math.PI * 2);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(12, 11); ctx.lineTo(12, 16);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(12, 8, 0.8, 0, Math.PI * 2);
                ctx.fill();
            } else {
                // Default dot
                ctx.arc(12, 12, 4, 0, Math.PI * 2);
                ctx.fill();
            }

            ctx.restore();
        }
    }

    onColorChanged: canvas.requestPaint()
    onNameChanged: canvas.requestPaint()
    onSizeChanged: canvas.requestPaint()
}

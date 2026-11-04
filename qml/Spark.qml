import QtQuick
import IISPH.Theme

// Mean-density trace. Values are real kg/m^3, the dashed line marks the rest
// density rho0 (labelled, so the reader never has to guess its value), and the
// curve itself is coloured by density error: green while the solver holds the
// constraint, amber while it is recovering, red when it has lost it.
Item {
    id: root
    property var points: []
    property real referenceDensity: 1000

    // Error bands as fractions (0.001 = 0.10 %). A converged IISPH/DFSPH run
    // sits around 0.010 %, i.e. an order of magnitude inside the green band, so
    // normal operation is green and only real transients light up.
    readonly property real goodLimit: 0.001
    readonly property real warnLimit: 0.010

    readonly property var c: Theme.colors
    readonly property var m: Theme.metrics
    readonly property color gridColor: c.border
    // The scale labels sit above/below the plot instead of in a left-hand
    // gutter, so the trace gets the full card width.
    readonly property real plotLeft: 4
    readonly property real plotRight: width - 4
    readonly property real plotTop: 13
    readonly property real plotBottom: height - 13

    property int hoverIndex: -1

    readonly property real dataMin: {
        if (points.length === 0) return referenceDensity;
        let v = points[0].y;
        for (let i = 1; i < points.length; ++i) v = Math.min(v, points[i].y);
        return v;
    }
    readonly property real dataMax: {
        if (points.length === 0) return referenceDensity;
        let v = points[0].y;
        for (let i = 1; i < points.length; ++i) v = Math.max(v, points[i].y);
        return v;
    }
    readonly property real dataSpan: Math.max(1e-3, dataMax - dataMin)
    // rho0 is the reference, but it must not flatten the plot: when the mean
    // density sits far away from it, the axis follows the data and the
    // reference is reported as off-scale instead of squeezing the trace.
    readonly property bool includeReference: referenceDensity >= dataMin - dataSpan * 2.5
                                            && referenceDensity <= dataMax + dataSpan * 2.5
    readonly property real valueLow: {
        const lo = includeReference ? Math.min(dataMin, referenceDensity) : dataMin;
        const hi = includeReference ? Math.max(dataMax, referenceDensity) : dataMax;
        return lo - Math.max(0.02, hi - lo) * 0.14;
    }
    readonly property real valueHigh: {
        const lo = includeReference ? Math.min(dataMin, referenceDensity) : dataMin;
        const hi = includeReference ? Math.max(dataMax, referenceDensity) : dataMax;
        return hi + Math.max(0.02, hi - lo) * 0.14;
    }
    // Zoomed far in, two decimals would print the same number twice.
    readonly property int axisDecimals: {
        const span = valueHigh - valueLow;
        if (span >= 1.0) return 2;
        if (span >= 0.01) return 3;
        return 4;
    }
    readonly property bool useTime: points.length > 1 && (points[points.length - 1].x - points[0].x) > 2.0
    readonly property real hoverX: hoverIndex >= 0 ? xForIndex(hoverIndex) : 0
    readonly property real hoverY: hoverIndex >= 0 ? yForValue(points[hoverIndex].y) : 0
    readonly property var hoverPoint: hoverIndex >= 0 && hoverIndex < points.length
                                     ? points[hoverIndex] : null
    readonly property color currentBand: bandColor(points.length > 0
                                                  ? Math.abs(points[points.length - 1].e !== undefined
                                                             ? points[points.length - 1].e : 0) : 0)

    readonly property bool refVisible: referenceDensity >= valueLow && referenceDensity <= valueHigh
    readonly property real refY: yForValue(referenceDensity)

    function bandColor(error) {
        const e = Math.abs(error);
        if (e >= root.warnLimit) return root.c.danger;
        if (e >= root.goodLimit) return root.c.warn;
        return root.c.ok;
    }

    function xForIndex(index) {
        if (points.length < 2) return plotLeft;
        if (!useTime)
            return plotLeft + (plotRight - plotLeft) * index / (points.length - 1);
        const tLast = points[points.length - 1].x;
        const tFirst = points[0].x;
        // Until the trace is 10 s long it stretches across the full width; after
        // that a 10 s window slides so the newest sample stays pinned right.
        const windowSpan = Math.max(1e-6, Math.min(10, tLast - tFirst));
        const nx = Math.max(0, 1 - (tLast - points[index].x) / windowSpan);
        return plotLeft + (plotRight - plotLeft) * nx;
    }

    function yForValue(value) {
        const span = Math.max(1e-6, valueHigh - valueLow);
        const norm = Math.max(0, Math.min(1, (value - valueLow) / span));
        return plotBottom - norm * (plotBottom - plotTop);
    }

    function nearestIndex(mx) {
        if (points.length === 0) return -1;
        let best = 0;
        let bestDist = Number.POSITIVE_INFINITY;
        for (let i = 0; i < points.length; ++i) {
            const d = Math.abs(xForIndex(i) - mx);
            if (d < bestDist) { bestDist = d; best = i; }
        }
        return best;
    }

    Canvas {
        id: plot
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const w = width;
            const h = height;
            if (w <= 2 || h <= 2) return;

            const left = root.plotLeft;
            const right = root.plotRight;
            const top = root.plotTop;
            const bottom = root.plotBottom;

            ctx.strokeStyle = Qt.rgba(root.gridColor.r, root.gridColor.g, root.gridColor.b, 0.55);
            ctx.lineWidth = 1;
            for (let i = 0; i < 3; ++i) {
                const y = top + (bottom - top) * i / 2;
                ctx.beginPath();
                ctx.moveTo(left, y);
                ctx.lineTo(right, y);
                ctx.stroke();
            }

            if (root.refVisible) {
                const ry = root.refY;
                ctx.save();
                ctx.setLineDash([4, 4]);
                ctx.strokeStyle = Qt.rgba(root.gridColor.r, root.gridColor.g, root.gridColor.b, 0.85);
                ctx.beginPath();
                ctx.moveTo(left, ry);
                ctx.lineTo(right, ry);
                ctx.stroke();
                ctx.restore();
            }

            if (root.points.length < 2) return;

            // Area fill takes the colour of the newest sample, so the whole
            // plot tints with the current error state.
            const band = root.currentBand;
            ctx.beginPath();
            for (let i = 0; i < root.points.length; ++i) {
                const x = root.xForIndex(i);
                const y = root.yForValue(root.points[i].y);
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.lineTo(right, bottom);
            ctx.lineTo(left, bottom);
            ctx.closePath();
            const grad = ctx.createLinearGradient(0, top, 0, bottom);
            grad.addColorStop(0, Qt.rgba(band.r, band.g, band.b, 0.22));
            grad.addColorStop(1, Qt.rgba(band.r, band.g, band.b, 0.0));
            ctx.fillStyle = grad;
            ctx.fill();

            // Line is stroked per segment: each piece carries the error band of
            // its own sample, so a spike is visibly red where it happened.
            ctx.lineWidth = 1.6;
            ctx.lineJoin = "round";
            ctx.lineCap = "round";
            for (let i = 1; i < root.points.length; ++i) {
                const prev = root.points[i - 1];
                const cur = root.points[i];
                const err = cur.e !== undefined ? cur.e : 0;
                const col = root.bandColor(err);
                ctx.beginPath();
                ctx.strokeStyle = col;
                ctx.moveTo(root.xForIndex(i - 1), root.yForValue(prev.y));
                ctx.lineTo(root.xForIndex(i), root.yForValue(cur.y));
                ctx.stroke();
            }
        }
    }

    // --- rho0 marker: dashed rule + its own value label ---------------------
    Rectangle {
        visible: root.refVisible
        x: root.plotLeft + 4
        y: Math.max(root.plotTop, root.refY - 16)
        width: refLabel.implicitWidth + 8
        height: 14
        radius: root.m.square ? 0 : 2
        color: root.c.card
        opacity: 1.0

        Text {
            id: refLabel
            anchors.centerIn: parent
            // UI font (not the mono number face): Consolas has no U+2080, so
            // "ρ₀" rendered as an empty box there.
            text: "ρ₀ = " + Number(root.referenceDensity).toFixed(0)
            color: root.c.fg3
            font.pixelSize: 9
        }
    }

    Text {
        x: 3
        y: 0
        text: root.valueHigh.toFixed(root.axisDecimals)
        color: root.c.fg3
        font.pixelSize: 9
        font.family: root.m.numberFamily
    }
    Text {
        x: 3
        y: root.height - 12
        text: root.valueLow.toFixed(root.axisDecimals)
        color: root.c.fg3
        font.pixelSize: 9
        font.family: root.m.numberFamily
    }
    // Off-scale reference: say where rho0 went instead of stretching the axis
    // to reach it.
    Text {
        visible: !root.includeReference
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.bottom: parent.bottom
        text: "ρ₀ " + Number(root.referenceDensity).toFixed(0)
              + (root.referenceDensity < root.valueLow ? " below" : " above")
        color: root.c.fg3
        font.pixelSize: 9
    }

    Rectangle {
        visible: root.hoverIndex >= 0 && root.points.length > 1
        x: root.hoverX
        y: root.plotTop
        width: 1
        height: root.plotBottom - root.plotTop
        color: Qt.rgba(root.c.fg3.r, root.c.fg3.g, root.c.fg3.b, 0.38)
    }

    Rectangle {
        visible: root.hoverIndex >= 0 && root.points.length > 1
        x: root.hoverX - 3
        y: root.hoverY - 3
        width: 6
        height: 6
        radius: root.m.square ? 0 : 3
        color: root.hoverPoint
               ? root.bandColor(root.hoverPoint.e !== undefined ? root.hoverPoint.e : 0)
               : root.currentBand
        border.width: 1
        border.color: root.c.card
    }

    Rectangle {
        id: tip
        visible: root.hoverIndex >= 0 && root.points.length > 1
        x: Math.max(2, Math.min(root.width - width - 2, root.hoverX + 10))
        y: 2
        width: 186
        height: 66
        radius: root.m.square ? 0 : root.m.radiusSm
        color: root.c.card
        border.width: 1
        border.color: root.c.borderStrong
        opacity: 0.97

        Column {
            anchors.fill: parent
            anchors.margins: 7
            spacing: 2

            Text {
                text: root.hoverPoint
                      ? "t " + Number(root.hoverPoint.x).toFixed(2) + " s  ·  step "
                        + (root.hoverPoint.step !== undefined ? root.hoverPoint.step : "—")
                      : ""
                color: root.c.fg2
                font.pixelSize: 10
                font.family: root.m.numberFamily
            }
            Text {
                text: root.hoverPoint
                      ? "ρ  " + Number(root.hoverPoint.y).toFixed(2) + " kg/m³" : ""
                color: root.c.fg
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.family: root.m.numberFamily
            }
            Text {
                text: root.hoverPoint && root.hoverPoint.e !== undefined
                      ? "Δρ  " + (Number(root.hoverPoint.e) * 100).toFixed(3) + " %" : ""
                color: root.hoverPoint
                       ? root.bandColor(root.hoverPoint.e !== undefined ? root.hoverPoint.e : 0)
                       : root.c.fg3
                font.pixelSize: 10
                font.family: root.m.numberFamily
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        onPositionChanged: root.hoverIndex = root.nearestIndex(mouseX)
        onExited: root.hoverIndex = -1
    }

    onPointsChanged: {
        plot.requestPaint();
        if (hoverIndex >= points.length) hoverIndex = -1;
    }
    onWidthChanged: plot.requestPaint()
    onHeightChanged: plot.requestPaint()
    onValueLowChanged: plot.requestPaint()
    onValueHighChanged: plot.requestPaint()
    onReferenceDensityChanged: plot.requestPaint()
    onCChanged: plot.requestPaint()
}

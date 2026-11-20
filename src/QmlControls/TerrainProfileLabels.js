/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

.pragma library

// RotorLab GC 5.1.2 (G78): label thinning for the Plan view terrain profile (TerrainStatus.qml).
// With one index label per mission item, a long plan (300 waypoints) drew the labels on top of
// each other as a solid strip. This picks which labels to show so that no two shown labels come
// closer than minSpacing pixels edge to edge, always keeping the first, the last and the forced
// (selected) ones.
//
// positions:  x in pixels per label slot (the label's centre), in flight-path order (so not
//             decreasing); NaN or undefined = the slot has no label
// forced:     true per slot that must be shown (the selected item)
// minSpacing: minimum gap in pixels between the edges of two shown labels
// widths:     optional, the width in pixels of each slot's label (a 3-digit number is wider than
//             a 1-digit one); without it every label counts as 0 wide (centre-to-centre spacing)
// returns:    an array of booleans, true = show the label in that slot
//
// Forced labels are always shown, even if they are closer than minSpacing to each other.
function thinLabels(positions, forced, minSpacing, widths) {
    var count = positions.length
    var shown = new Array(count)
    var keep = new Array(count)
    var first = -1
    var last = -1

    for (var i = 0; i < count; i++) {
        shown[i] = false
        keep[i] = false
        if (!_hasLabel(positions[i])) {
            continue
        }
        if (first === -1) {
            first = i
        }
        last = i
        keep[i] = forced[i] === true
    }
    if (first === -1) {
        return shown
    }
    keep[first] = true
    keep[last] = true

    // The next kept label after each slot, so a thinned label never crowds it.
    var nextKept = new Array(count)
    var next = -1
    for (i = count - 1; i >= 0; i--) {
        nextKept[i] = next
        if (keep[i]) {
            next = i
        }
    }

    var lastShown = -1
    for (i = 0; i < count; i++) {
        if (!_hasLabel(positions[i])) {
            continue
        }
        if (keep[i] ||
                ((lastShown === -1 || _clear(positions, widths, lastShown, i, minSpacing)) &&
                 (nextKept[i] === -1 || _clear(positions, widths, i, nextKept[i], minSpacing)))) {
            shown[i] = true
            lastShown = i
        }
    }
    return shown
}

// True if the labels in slots a < b are at least minSpacing apart, edge to edge.
function _clear(positions, widths, a, b, minSpacing) {
    return positions[b] - positions[a] >= (_width(widths, a) + _width(widths, b)) / 2 + minSpacing
}

function _width(widths, i) {
    var w = widths ? widths[i] : 0
    return typeof w === "number" && isFinite(w) ? w : 0
}

function _hasLabel(x) {
    return typeof x === "number" && isFinite(x)
}

/****************************************************************************
 *
 * RotorLab: A156. One full-window page layout for every GC settings page.
 *
 * A vertically scrolling page whose column fills the window minus the
 * standard settings padding (two character widths each side), so sections
 * stretch with the window instead of sitting in a narrow centered column.
 * A scroll bar shows whenever the content is taller than the page.
 * The page also carries the shared sizing rules and colors, so every page
 * reflows and reads the same way:
 *
 *  - Labelled rows: the label column is labelColumnWidth(rowWidth), 40% of
 *    the row up to about 40 character widths; the field follows right after
 *    it, up to fieldMaxWidthFor(rowWidth, spacing) (48 character widths at
 *    most). The rest of the row stays empty. Use these as a maximum or
 *    preferred width, never a minimum, so a field still shrinks when the
 *    window does.
 *  - narrow: true when the column is narrower than about 110 character
 *    widths (about 1000 px at 100% scaling). Labelled rows then put the field
 *    under its label instead of beside it.
 *  - noteColor, warnColor, errorColor, okColor: notes, captions and status
 *    lines at 4.5:1 or better on the card, in light and dark.
 *  - inputColor, inputTextColor, inputBorderColor, focusColor,
 *    placeholderColor: text entries in the theme's input colors, with a
 *    visible focus outline.
 *  - SettingsGroupLayout headings inside this page show their description at
 *    body size (bodySizeNotes); elsewhere, such as the flight-view popups,
 *    they keep the small size.
 *
 * Page content is placed in the column (default property contentItem), so a
 * child that should stretch sets Layout.fillWidth: true.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Palette
import QGroundControl.ScreenTools

Item {
    id: control

    default property alias contentItem: _column.data

    property alias  spacing:        _column.spacing
    property alias  flickable:      _flick
    property real   pagePadding:    ScreenTools.defaultFontPixelWidth * 2
    readonly property real columnWidth: _column.width

    /// Read by SettingsGroupLayout: notes and descriptions on this page are
    /// at least body size.
    readonly property bool bodySizeNotes: true

    readonly property real fieldMinWidth:       ScreenTools.defaultFontPixelWidth * 20
    readonly property real fieldMaxWidth:       ScreenTools.defaultFontPixelWidth * 48
    readonly property real labelColumnMaxWidth: ScreenTools.defaultFontPixelWidth * 40
    readonly property real stackBelowWidth:     ScreenTools.defaultFontPixelWidth * 110
    readonly property bool narrow:              _column.width < stackBelowWidth

    /// The label column of a labelled row: 40% of the row, up to about 40
    /// character widths.
    function labelColumnWidth(rowWidth) {
        return Math.min(rowWidth * 0.4, labelColumnMaxWidth)
    }

    /// The widest a field may be: right after the label column, up to 48
    /// character widths. On a narrow page the field is under its label.
    function fieldMaxWidthFor(rowWidth, spacing) {
        if (narrow) {
            return fieldMaxWidth
        }
        return Math.max(0, Math.min(fieldMaxWidth, rowWidth - labelColumnWidth(rowWidth) - spacing))
    }

    /// About half the row, between fieldMinWidth and fieldMaxWidth.
    function fieldWidth(rowWidth) {
        return Math.max(fieldMinWidth, Math.min(fieldMaxWidth, rowWidth * 0.5))
    }

    // Notes, captions and status lines stay readable in both themes, at least
    // 4.5:1 on the card. The stock gray, orange and green are too pale on the
    // light card, and the stock red and orange on the dark one.
    readonly property bool  lightTheme: _pal.globalTheme === QGCPalette.Light
    readonly property color noteColor:  lightTheme ? "#39414b" : "#d4d4d4"
    readonly property color warnColor:  lightTheme ? "#8a4a00" : "#e89020"
    readonly property color errorColor: lightTheme ? _pal.colorRed : "#ff7a7a"
    readonly property color okColor:    lightTheme ? "#006b21" : _pal.colorGreen

    // Text entries: the theme's input colors, a blue focus outline, and
    // placeholder text at about 5:1 on the input.
    readonly property color inputColor:         _pal.textField
    readonly property color inputTextColor:     _pal.textFieldText
    readonly property color inputBorderColor:   _pal.buttonBorder
    readonly property color focusColor:         "#4D8BFF"
    readonly property color placeholderColor:   lightTheme ? "#5b6573" : "#8b97a6"

    QGCPalette { id: _pal; colorGroupEnabled: true }

    // A plain Flickable with a scroll bar that is always shown while the
    // content is taller than the page, so no page runs past the bottom
    // without one.
    Flickable {
        id:                     _flick
        anchors.fill:           parent
        contentWidth:           width
        contentHeight:          _column.height + ScreenTools.defaultFontPixelHeight * 2
        boundsBehavior:         Flickable.StopAtBounds
        flickableDirection:     Flickable.VerticalFlick
        clip:                   true
        maximumFlickVelocity:   (ScreenTools.realPixelDensity * 25.4) * 8

        ScrollBar.vertical: ScrollBar {
            policy: _flick.contentHeight > _flick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            contentItem: Rectangle {
                implicitWidth:  ScreenTools.defaultFontPixelWidth * 0.75
                radius:         width / 2
                color:          _pal.text
                opacity:        0.5
            }
        }

        ColumnLayout {
            id:         _column
            x:          control.pagePadding
            y:          ScreenTools.defaultFontPixelHeight
            width:      Math.max(0, _flick.width - control.pagePadding * 2)
            spacing:    ScreenTools.defaultFontPixelHeight
        }
    }
}

# GUI scale

Open **View → Interface → GUI scale** in the Configuration Editor. Choose a
preset from 50% to 300%, or **Custom...** for an integer percentage in that
range. Restart when prompted, or choose No and apply it on the next launch.
The Device Selector follows the same preference on its next launch.

The percentage multiplies Windows' per-monitor display scale. At Windows 150%
and GUI 125%, the effective scale is 187.5%. Text, controls, cards and graphs
scale together. Native plugin panels receive the effective device pixel ratio
through the host's existing VST scaling support; individual plugins may ignore
that request.

**System default** removes the saved override and restores Qt's normal display
scaling, including any `QT_SCALE_FACTOR` inherited from the launching process.
The default is preserved across the Editor's in-process restart. The preference
is `interface/guiScalePercent` in the existing per-user Editor settings. Invalid
or out-of-range stored values use System default. Resetting global preferences
also resets this choice.

The implementation sets Qt's documented `QT_SCALE_FACTOR` before constructing
QApplication, including after a settings restart. See [Qt High DPI](https://doc.qt.io/qt-6/highdpi.html).

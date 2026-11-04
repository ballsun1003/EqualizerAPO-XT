# Follow the Windows default playback device

Select **Default (...)** in the Configuration Editor's device list. While the
Editor remains open, changing Windows' default playback device refreshes that
entry, the device format badge, channel information, every open configuration
tab and the analysis context. A manually selected endpoint stays selected.
An explicit channel-layout override is retained; **From device** follows the
new endpoint's channel layout.

If no playback endpoint is available, the entry becomes **Default (no playback
device)**. It follows the next default endpoint without restarting the Editor.
This updates the Editor's view and analysis; it does not install the APO on an
endpoint or rewrite `Device:` commands in configuration files.

The Editor registers an `IMMNotificationClient` for the render/multimedia role,
the role `DeviceAPOInfo` already uses. Callbacks queue work onto the Qt thread.
A 100 ms debounce coalesces notifications, endpoint reads run on the existing
background probe worker, and generation checks discard stale results. Startup
also refreshes after registration to close the initial enumeration race.

`DefaultDeviceWatcherTests` injects a fake enumerator, sends notifications from
a worker thread, and checks role filtering, null endpoints, Qt-thread delivery,
queued/late callbacks after destruction, reference balance and registration
failure. Manual end-to-end verification: keep two config tabs open, select
Default, switch Windows playback endpoints, check the name/channels/analysis,
then repeat with a specifically selected endpoint and with all output devices
temporarily unavailable. These machine-wide device changes are not performed
by the automated tests.

Local x64 AVX2 validation passed the Editor build and all 4,184
`EditorLogicTests` checks, including this notification client's lifetime and
cross-thread delivery checks. Physical endpoint switching remains a manual
acceptance check before an installed release.

See [Microsoft's device event contract](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events).

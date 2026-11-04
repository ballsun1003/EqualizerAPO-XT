# Choose an effect inside a VST3 module

Load the VST3 file, then choose **Effect** on the card or legacy row. Modules
with one effect need no extra selection. For WaveShell, choose the installed
Nx variant you intend to use, for example **NX Stereo** for stereo input.
Opening or embedding the panel uses that same effect instance.

The saved line contains `ClassId` followed by the effect's 32 hexadecimal
VST3 CID. Names and factory indices are not identities. For example:

```text
VSTPlugin: Library "C:\Program Files\Common Files\VST3\WaveShell1-VST3 17.1_x64.vst3" ClassId 5653544E5853536E782073746572656F Input Stereo Output Stereo
```

This CID was measured for NX Stereo in WaveShell 17.1; enumerate your installed
module instead of assuming that a particular variant exists. Changing the bus
layout alone does not change Ambisonics into Stereo.

Changing Effect closes the old panel, clears its chunk/parameters and channel
layout/fill, creates the selected class, and reopens an embedded panel if it
was embedded before. Choose the new class's bus layouts and channel fill after
switching. Modules remain shared; effect selection and state belong to each
instance. Replacing a VST3 module also discards the old class's state, because
a truncated VST2 unique ID cannot establish VST3 state compatibility.

Existing lines without `ClassId` still load the first Audio Effect. Saving a
successfully loaded row pins that effect's CID. An explicit missing CID is an
error and never falls back to a different effect. A malformed CID is rejected
by the parser. The engine skips an invalid line, leaving its input unchanged.
`ClassId` is for VST3; remove it when intentionally switching to a VST2 DLL.

## Diagnostics and regression coverage

```powershell
VstPreviewProbe.exe --list-classes 'C:\path\WaveShell.vst3'
VstPreviewProbe.exe --plugin 'C:\path\WaveShell.vst3' --class-id <CID> --seconds 1 --expect-nonsilent
```

The first command prints CID, effect name and subcategories without creating
an effect. The second processes buffers inside the probe; it does not replace
the installed APO or play a test tone through the system output.

`HybridConvTests` uses two effect classes in the same fixture module, including
two CIDs with colliding first 32 bits. It covers enumeration, simultaneous
instances, saved-state restoration, L-only/R-only routing, non-first-class
engine construction, missing IDs and parser round trips. `Editor.exe
--selftest-vst` also checks that the legacy row preserves CID and state.

Local validation on 2026-11-03 found two MaxxBass classes in WaveShell 14.12 and
ten Nx classes in WaveShell 17.1. Selecting NX Stereo by CID initialized a
2-input/2-output float processor, processed 48,128 frames with finite nonzero
output, and saved a 5,592-character state chunk. This is an in-process test;
system audio through a newly installed APO still needs a separate check.

# ReWasp

**The original Wasp synth, back in 64-bit FL Studio.**

FL Studio's classic Wasp only ever existed as a 32-bit plugin. Modern FL Studio is
64-bit only, so Wasp just shows *"This plugin doesn't have a 64 bit version"* and old
projects lose their Wasp sounds.

ReWasp fixes that. Install it once and:

- Wasp shows up and works in 64-bit FL Studio like any other plugin
- old projects open with their Wasp channels and patches intact
- the sound is the original Wasp's, bit for bit
- automation and the knob right-click menu (edit events, automation clips, link to
  controller) work, keyboard shortcuts included

## Install

1. Download **ReWasp-Setup.exe** from [Releases](https://github.com/MickeyFLStudio/rewasp/releases).
2. Close FL Studio and run it.

That's it. The installer finds your FL Studio versions by itself.

**You need the original 32-bit Wasp from FL Studio 12 or older** (tested with FL Studio
11's; FL Studio 8 and 12 ship the same kind of build). ReWasp doesn't include Wasp (it's Image-Line's); it uses the copy already on your
PC. If the installer can't find one, install an older FL Studio alongside your current one
(you can keep several versions installed), or point the installer at a `Wasp.dll` that has
its `Artwork` folder next to it. The Wasp build that came with FL Studio 20 isn't supported
yet.

**After an FL Studio update**, run ReWasp-Setup again: updates can put Image-Line's
placeholder back.

**To remove it**: Windows Settings > Apps > ReWasp > Uninstall. FL Studio goes back
exactly as it was.

## How it works

A 64-bit program can't load a 32-bit plugin, so ReWasp splits Wasp in two:

```
FL Studio (64-bit)
  └─ Wasp_x64.dll          ReWasp's 64-bit plugin, in place of Image-Line's placeholder
       │  shared memory + window messages
       ▼
WaspHost32.exe (32-bit)   one small helper per Wasp channel
  └─ Wasp.dll              the original Wasp, unchanged
```

FL Studio loads ReWasp's `Wasp_x64.dll` under Wasp's own name, so projects find it.
For every Wasp channel it starts a `WaspHost32.exe`, which loads the real 32-bit Wasp
and plays the role of FL Studio for it. Notes, knob moves, automation, audio, saving and
the editor window are passed across in both directions; Wasp's window sits inside FL's
plugin window. You'll see one `WaspHost32.exe` per Wasp in Task Manager: that's normal.
If a helper ever dies or stops answering, the plugin starts a new one by itself within a
second or two and puts the patch, settings and window back; a channel is never left dead.

The installer:

- finds every FL Studio install (Windows' list of installed programs + `Program Files\Image-Line`)
- only replaces a `Wasp_x64.dll` that is Image-Line's placeholder (or an older ReWasp);
  anything else is left alone
- keeps the placeholders and restores them when you uninstall

## Build it yourself

Needs Visual Studio with *Desktop development with C++*, and Inno Setup 6 for the installer.

```
build.bat
```

Output: `build\x64\Wasp_x64.dll`, `build\x86\WaspHost32.exe`,
`installer\Output\ReWasp-Setup-<version>.exe`, and `build\x64\test_host64.exe`, a test
host that loads the plugin the way FL does and renders a note
(`test_host64.exe path\to\Wasp_x64.dll`).

| File | What it is |
|---|---|
| `src/native64.cpp` | the 64-bit FL plugin (`Wasp_x64.dll`) |
| `src/helper32.cpp` | the 32-bit helper (`WaspHost32.exe`) |
| `src/flhost.cpp` | the FL Studio side as Wasp sees it: voices, timing, host calls |
| `src/ipc.h` | the shared-memory protocol between the two |
| `installer/rewasp.ps1` | install / uninstall logic |
| `sdk/` | Image-Line's FL Studio plugin SDK headers |

## Notes

- Wasp XT isn't bridged.
- Windows only (like FL Studio's native plugins).
- ReWasp is an independent project, not made or supported by Image-Line. Wasp and FL
  Studio are Image-Line's. ReWasp doesn't distribute any Image-Line plugin.

## Credits

Made by **Mickey Perry**, aka **Pafu Igniters** (breakbeat, trance and drum & bass), who
also does motion design and animation. Wasp is all over my old tracks; I hope ReWasp
brings some of yours back too.

- Music: [SoundCloud](https://soundcloud.com/pafu) / [Bandcamp](https://pafu.bandcamp.com)
- Motion design and animation: [mickeyperry.github.io](https://mickeyperry.github.io/)

MIT license.

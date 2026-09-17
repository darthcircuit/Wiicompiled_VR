# WiiCompiled VR for Meta Quest (Android)

Standalone Android/OpenXR build of the Mario Kart Wii recompilation for Quest 2,
Quest 3, Quest 3S and Quest Pro. The full design, build walkthrough and current
status live in [docs/quest-port.md](../docs/quest-port.md); this directory only
holds the Gradle project, its helper scripts, the game kit tooling
(`QuestGameKit.psm1`, `Build-QuestGame.ps1`) and `nod-jni`.

```powershell
powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1          # stages the SDL3 3.4.4 AAR once
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Install               # base game, debug-signed, installs over adb
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Flavor retroRewind    # Retro Rewind product
```

The translated game (the translator's `generated/` tree for **your own**
PAL `RMCP01` disc) is passed with `-Generated <dir>`; it defaults to the
installer workspace next to this checkout. No game data is ever part of the
APK, and neither is any translated game code: the base APK carries a game kit,
and your `libmain.so` is built from your own disc and imported
(`Build-QuestGame.ps1 -Install`, or **Import from computer** with its `.wcgame`).
Copy your disc image to the headset (for example `adb push MarioKart.iso
/sdcard/Download/`) and press **Select disc image** in the launcher, which checks
and extracts it with nod, as the PC installer does. Or push an already extracted
disc to

```
/sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA
```

`Config.toml`, saves and logs live in the same `WiiCompiledOpenXRVR` directory.

The app opens on a 2D launcher panel modelled on the PC launcher (WheelWizard VR):
**Home** starts the game in the headset and says when DATA is missing, and
**Settings** edits `Config.toml` (VR camera, render scale, virtual screen,
resolution, controllers, audio). The game itself is `QuestActivity`, in its own
`:game` process; `adb shell am start -n org.wiicompiled.quest/.QuestActivity`
still starts it directly.

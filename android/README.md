# WiiCompiled VR for Meta Quest (Android)

Standalone Android/OpenXR build of the Mario Kart Wii recompilation for Quest 2,
Quest 3, Quest 3S and Quest Pro. The full design, build walkthrough and current
status live in [docs/quest-port.md](../docs/quest-port.md); this directory only
holds the Gradle project and its two helper scripts.

```powershell
powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1          # stages the SDL3 3.4.4 AAR once
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Install               # base game, debug-signed, installs over adb
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Flavor retroRewind    # Retro Rewind product
```

The translated game (the translator's `generated/` tree for **your own**
PAL `RMCP01` disc) is passed with `-Generated <dir>`; it defaults to the
installer workspace next to this checkout. No game data is ever part of the
APK: push the extracted disc afterwards to

```
/sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA
```

`Config.toml`, saves and logs live in the same `WiiCompiledOpenXRVR` directory.

# random-vr-game

A minimal "Hello World" immersive VR app for Meta Quest headsets, packaged
as an APK.

It's a native OpenXR app (C++, NDK, `android.app.NativeActivity` — no
Kotlin/Java UI, no game engine). On launch it opens a real OpenXR session
and renders a big sign reading "SKELETON" floating about 2.5m in front of
where you started, head-tracked in stereo. The sign's pixels are generated
at startup from a small hand-drawn bitmap font baked directly into the C++
code — no font file, no Android Canvas/JNI round-trip.

It uses the Khronos-published `openxr_loader_for_android` (from Maven
Central) to talk to whatever OpenXR runtime Quest's system software
provides — no gated Meta SDK download required.

## Build locally

Requires JDK 17 and the Android SDK with:
- `platforms;android-34` and a recent build-tools version
- `ndk;26.1.10909125`
- `cmake;3.22.1`

```bash
./gradlew assembleDebug
```

The APK is produced at `app/build/outputs/apk/debug/app-debug.apk`.

## Build via GitHub Actions

Pushing to any branch runs `.github/workflows/build-apk.yml`, which installs
the Android SDK/NDK/CMake and builds the debug APK on a hosted runner,
uploading it as the `hello-meta-quest-debug-apk` workflow artifact — useful
when you don't have the Android SDK installed locally.

## Install on a Meta Quest headset

1. Enable Developer Mode on the headset via the Meta Horizon mobile app.
2. Connect the headset over USB and authorize the connection.
3. Install with `adb`:

   ```bash
   adb install app-debug.apk
   ```

4. Find "Hello Meta Quest" under the headset's **Unknown Sources** /
   **App Library** (filter by "Unknown Sources") and launch it. You should
   be dropped into an immersive view with a big "SKELETON" sign floating in
   front of you — that's the app running.

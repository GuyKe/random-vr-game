# random-vr-game

A minimal "Hello World" Android app for Meta Quest headsets, packaged as an APK.

It runs as a standard 2D Android app in a floating panel inside the Quest's
Home environment — no game engine or native VR SDK required.

## Build locally

Requires JDK 17 and the Android SDK (`ANDROID_HOME` set, with platform 34 and
a recent build-tools version installed).

```bash
./gradlew assembleDebug
```

The APK is produced at `app/build/outputs/apk/debug/app-debug.apk`.

## Build via GitHub Actions

Pushing to any branch runs `.github/workflows/build-apk.yml`, which builds the
debug APK on a hosted runner and uploads it as the `hello-meta-quest-debug-apk`
workflow artifact — useful when you don't have the Android SDK installed
locally.

## Install on a Meta Quest headset

1. Enable Developer Mode on the headset via the Meta Horizon mobile app.
2. Connect the headset over USB and authorize the connection.
3. Install with `adb`:

   ```bash
   adb install app-debug.apk
   ```

4. Find "Hello Meta Quest" under the headset's **Unknown Sources** /
   **App Library** (filter by "Unknown Sources") to launch it.

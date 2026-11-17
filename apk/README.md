# panicast Remote — Android APK

Native Kotlin + Jetpack Compose + Material3 client for panicast (N line). Auto-discovers panicast
players on the LAN, pairs with a PIN (dynamic, shown in panicast via `:pin`, or universal **6696**
for headless), and gives full control + live status over the PRP TCP protocol.

**No runtime dependencies** — all libraries (Compose) are bundled by Gradle. minSdk 24 (Android 7.0+).

## Build (Android Studio — recommended)

1. Install **Android Studio** (Hedgehog or newer) with **Android SDK 34** + **JDK 17**.
2. Open the `apk/` folder as a project. Android Studio generates the Gradle wrapper + syncs.
3. Connect an Android device (USB debugging) or start an emulator.
4. **Run ▶** → installs the debug APK.
5. For a signed release APK: **Build → Generate Signed Bundle / APK → APK**, create a keystore,
   pick `release`. Output: `app/build/outputs/apk/release/app-release.apk`.

## Build (command line)

```bash
cd apk
gradle wrapper            # one-time, if gradlew is absent
./gradlew assembleRelease # or assembleDebug
# APK: app/build/outputs/apk/release/app-release.apk
```
Requires ANDROID_HOME pointing at the Android SDK + JDK 17.

## Usage

1. On the panicast host: set `[remote] enable=true` and `bind=0.0.0.0` in `~/.config/podradio/config.ini`; restart panicast. Note the PIN (`:pin` in panicast).
2. Open the app → **Scan network for players** → tap the discovered panicast.
3. Enter the PIN (or `6696`) → control screen (play/pause, next/prev, seek, volume, speed, mode, navigation, play-mode) with live status via `idle`.

## Architecture

- `Discovery.kt` — UDP broadcast `PANICAST_DISCOVER` → collects `PANICAST 1 tcp=.. ws=..` responses.
- `PrpClient.kt` — TCP line-protocol client (PRP): commands, `password` PIN auth, `idle` subscription, `status` polling.
- `MainActivity.kt` — Compose UI: Scan → PIN → Control screens. Material3.

Server requirements: panicast N04+ with `[remote] enable=true` (default discovery port 18430).

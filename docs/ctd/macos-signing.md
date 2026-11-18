# Signing and notarizing CTD OBS for macOS

Every macOS build from CI (`.github/workflows/ctd-macos.yaml`, both the `macos-26` and the `macos-26-intel` job) is
signed with CineTech Digital's **Developer ID Application** certificate, **notarized** by Apple and **stapled**, as soon
as the seven repository secrets below exist. Testers then install it like any Mac app: no `xattr` command, no "is
damaged" message, and macOS keeps CTD OBS's camera, microphone and screen recording permissions from one build to the
next (they belong to the signing team and bundle id, `com.cinetechdigital.ctd-obs`, which no longer change).

Without `MACOS_CERT_P12_BASE64` (forks, or before the secrets are added) the workflow does exactly what it did before:
an ad hoc signature, no notarization, a zip only. The job summary says which of the two a run did.

The secrets are only read by the workflow. Agents never see or handle them, and nothing prints them (GitHub also masks
them in the log).

## The secrets

All are **repository secrets** of `ItsLimitlezz/ctd-obs`.

| Secret | What it is |
|---|---|
| `MACOS_CERT_P12_BASE64` | The Developer ID Application certificate **with its private key**, exported as `.p12`, base64 |
| `MACOS_CERT_PASSWORD` | The password chosen when exporting the `.p12` |
| `MACOS_KEYCHAIN_PASSWORD` | Any random string; it locks the temporary keychain CI creates for each run |
| `APPLE_TEAM_ID` | CineTech Digital's 10-character Apple team ID |
| `NOTARY_KEY_ID` | The App Store Connect API key's Key ID |
| `NOTARY_ISSUER_ID` | The App Store Connect API Issuer ID (a UUID) |
| `NOTARY_KEY_P8_BASE64` | The API key file (`AuthKey_<KeyID>.p8`), base64 |

## Juan's one-time setup

Needs the Apple Developer Program **Account Holder** (only the Account Holder can create a Developer ID certificate and
the first App Store Connect API key) and a Mac.

### 1. Create the Developer ID Application certificate

Either way works; Xcode is quicker.

- **Xcode:** Xcode → Settings → Accounts → select the Apple ID → the CineTech Digital team → **Manage Certificates…** →
  **+** → **Developer ID Application**. The certificate and its private key land in the login keychain.
- **developer.apple.com:**
  1. On the Mac, Keychain Access → Keychain Access menu → Certificate Assistant → **Request a Certificate From a
     Certificate Authority…**; your email, common name "CineTech Digital", **Saved to disk**. This also creates the
     private key in the login keychain.
  2. developer.apple.com → Account → **Certificates, Identifiers & Profiles** → Certificates → **+** →
     **Developer ID Application** → profile type **G2 Sub-CA** → upload the `.certSigningRequest` → Download.
  3. Double-click the downloaded `.cer` so Keychain Access pairs it with the private key.

### 2. Export it as .p12

1. Keychain Access → **login** keychain → **My Certificates**.
2. Find **Developer ID Application: CineTech Digital (XXXXXXXXXX)**. Expand it: a private key must be underneath (if
   not, the certificate was made on another Mac; export from that one).
3. Right-click the certificate → **Export…** → format **Personal Information Exchange (.p12)** → save as
   `ctd-developer-id.p12`.
4. Choose a strong password. That password is `MACOS_CERT_PASSWORD`.

The `XXXXXXXXXX` in the certificate's name is the team ID (`APPLE_TEAM_ID`). It's also under developer.apple.com →
Account → **Membership details** → Team ID.

### 3. Create the App Store Connect API key (for notarization)

1. appstoreconnect.apple.com → **Users and Access** → **Integrations** → **App Store Connect API** → **Team Keys**.
   (The first time, the Account Holder has to click **Request Access** and accept.)
2. **Generate API Key** (or **+**): name `CTD OBS notarization`, access **Developer** → **Generate**.
3. **Download** the key: `AuthKey_<KeyID>.p8`. Apple lets you download it **once**.
4. Note the **Key ID** (in the key's row) → `NOTARY_KEY_ID`, and the **Issuer ID** (above the table, **Copy**) →
   `NOTARY_ISSUER_ID`.

### 4. Add the secrets to GitHub

github.com/ItsLimitlezz/ctd-obs → **Settings** → **Secrets and variables** → **Actions** → **New repository secret**,
once per secret: the name exactly as in the table, then the value.

For the two files, copy the base64 text straight to the clipboard in Terminal and paste it as the value:

```sh
base64 -i ctd-developer-id.p12 | pbcopy        # → MACOS_CERT_P12_BASE64
base64 -i AuthKey_XXXXXXXXXX.p8 | pbcopy       # → NOTARY_KEY_P8_BASE64
openssl rand -base64 24 | pbcopy               # → MACOS_KEYCHAIN_PASSWORD (any random string)
```

Paste the others as plain text: `MACOS_CERT_PASSWORD`, `APPLE_TEAM_ID`, `NOTARY_KEY_ID`, `NOTARY_ISSUER_ID`.

Then keep the `.p12` (with its password) and the `.p8` in the password manager and delete the loose copies from the
Mac. Don't send them over chat or email, and never commit them.

### 5. Check it

1. github.com/ItsLimitlezz/ctd-obs → **Actions** → **CTD OBS macOS** → **Run workflow** on `ctd/main` (or push to a
   `ctd/**` branch: the **CTD OBS** run's macOS jobs, which also feed the release zip).
2. The run's summary says **macOS signing: Developer ID, notarized** with **Accepted** for the app and the disk image.
   The artifact now holds the zip and a `.dmg`.
3. On a Mac, after installing: `spctl -a -vvv "/Applications/CTD OBS.app"` prints `accepted` and
   `source=Notarized Developer ID`.

## Renewal and revoking

- A Developer ID Application certificate is valid for 5 years. Builds signed before it expires keep opening after it
  (each signature has Apple's secure timestamp). Before it expires, make a new one (steps 1–2) and replace
  `MACOS_CERT_P12_BASE64` and `MACOS_CERT_PASSWORD`.
- Revoke the certificate only if its private key leaked: revoking also stops **already shipped** builds signed with it
  from opening.
- The API key doesn't expire. If it leaks, revoke it in App Store Connect, make a new one (step 3) and replace the
  three `NOTARY_*` secrets. Revoking it doesn't affect shipped builds.

## What CI does

Steps in `.github/workflows/ctd-macos.yaml`, script `.github/scripts/ctd-macos-sign`:

1. **Set up signing**: with no `MACOS_CERT_P12_BASE64`, says "ad hoc" in the summary and stops there. Otherwise a
   temporary keychain in the runner's temp folder (create, unlock, import the `.p12` plus Apple's Developer ID G2
   intermediate, `set-key-partition-list` so `codesign` needs no prompt), and the identity whose name has
   `Developer ID Application` and `(APPLE_TEAM_ID)`.
2. **Package**: branding and bundle checks as before, then signing **inside out**, all with `--options runtime`
   (hardened runtime) and `--timestamp`: every Mach-O file in the bundle (Qt frameworks' binaries, dylibs, OBS's
   plugins and the CTD modules, CEF's libraries, `obs-ffmpeg-mux`), then every nested bundle deepest first (frameworks,
   `.plugin`s, CEF's four `OBS Helper` apps), then `CTD OBS.app`. Then `codesign --verify --deep --strict`, a check that
   the team and hardened runtime are right, and `spctl -a -vvv -t exec` (before notarization it says
   `Unnotarized Developer ID`, which is expected).
3. **Notarize**: the app (zipped with `ditto -c -k --keepParent`) goes to `xcrun notarytool submit --wait` with the API
   key; the notary log is printed every time, and anything but **Accepted** fails the job. Then
   `xcrun stapler staple`, `spctl` must say `accepted` / `source=Notarized Developer ID`, and the zip is made again
   from the stapled app with `ditto -c -k --sequesterRsrc --keepParent`. A disk image (the app plus an Applications
   link) is made, signed, notarized and stapled the same way.
4. **Remove signing keychain** (always, also after a failure): deletes the keychain and the key files.

Signing happens after the build, in a script, instead of through OBS's own Xcode hooks (`CODESIGN_IDENT` /
`CODESIGN_TEAM` in `build-macos`, `OBS_CODESIGN_IDENTITY` in `CMakePresets.json`): the Package step renames
`OBS.app` to `CTD OBS.app` and may correct its `Info.plist`, which would break a build-time signature; OBS's release
path for a Developer ID build is an Xcode archive/export, which CTD's CI doesn't use; and keeping the build identical
for signed and ad hoc runs keeps the Xcode compilation cache shared between them.

### Entitlements

The app is signed with OBS's own `frontend/cmake/macos/entitlements.plist`, unchanged from OBS Studio:

| Entitlement | Why |
|---|---|
| `com.apple.security.device.camera` | Video capture device sources |
| `com.apple.security.device.audio-input` | Audio input capture (microphones, interfaces) |
| `com.apple.security.cs.allow-unsigned-executable-memory` | LuaJIT (Lua scripts) and other code generated at runtime |
| `com.apple.security.cs.disable-library-validation` | Loads third-party OBS plugins and the user's own Python for Python scripts, which aren't signed by CTD's team |

CEF's helper apps keep obs-browser's entitlements (`plugins/obs-browser/cmake/macos/entitlements-helper*.plist`:
`allow-jit` for the renderer and GPU helpers, unsigned executable memory, library validation off). Not added:
`allow-jit` on the app itself (CEF's JIT runs in the helpers) and `automation.apple-events` (OBS sends no Apple
Events; global hotkeys use Accessibility). Screen and system audio recording need no entitlement, only the user's
permission.

### If notarization is rejected

Open the failed run → the **Notarize** step → **Notary log**. Each issue names the file and the reason. Usual causes:
a binary not signed, or signed without the hardened runtime or timestamp (a new kind of file in the bundle the signing
script didn't find), or a certificate that isn't **Developer ID Application** (an "Apple Development" or "Developer ID
Installer" certificate won't do).

## Virtual camera (not done yet)

The macOS virtual camera is a camera **system extension** inside the app, so it needs more than this certificate.
What Juan would need to request or create:

1. An explicit **App ID** `com.cinetechdigital.ctd-obs` with the **System Extension** capability and an **App Group**
   (OBS uses `<TeamID>.com.obsproject.obs-studio`; CTD's own group would be `<TeamID>.com.cinetechdigital.ctd-obs`).
2. An App ID for the camera extension, `com.cinetechdigital.ctd-obs.mac-camera-extension` (OBS's is
   `com.obsproject.obs-studio.mac-camera-extension`), with the same App Group.
3. A **Developer ID provisioning profile** for each App ID, as two more secrets for CI.
4. Then, in code: new virtual camera UUIDs for CTD OBS, the App Group renamed in the entitlements
   (`frontend/cmake/macos/entitlements-extension.plist`,
   `plugins/mac-virtualcam/src/camera-extension/cmake/macos/entitlements.plist`), `ENABLE_VIRTUALCAM` on in the
   `macos` preset, and signing with the profiles (OBS's Xcode archive/export path with
   `exportOptions-extension.plist`).

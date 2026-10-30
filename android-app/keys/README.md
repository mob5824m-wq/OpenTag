# Release signing key ("open key")

`opentag-release.p12` is the key the CI (`android-app` workflow) uses to sign
the release APKs published on every merge to `main`.

**Why a committed key?** The app is sideloaded from GitHub Releases, not
distributed via a store. Android refuses to install an update whose
signature differs from the installed version, so releases must be signed
with one *stable* key or every update would force an uninstall first. For an
open-source, sideloaded app this signature provides **upgrade continuity,
not authenticity**: the key is public, so anyone can build and sign an APK
that upgrades over the official one. Treat the APK's origin (this repo's
Releases page) as the real trust anchor.

- keystore: PKCS#12, alias `opentag`, store **and** key password `opentag`
- self-signed RSA-2048, valid ~30 years
- `app/build.gradle.kts` uses it by default; builds stay reproducible for
  anyone cloning the repo.

## Using a private key instead

If you ever want a closed signature, keep a keystore out of the repo and set
two GitHub Actions secrets on this repository:

- `OT_KEYSTORE_B64` — base64 of your PKCS#12
- `OT_KEYSTORE_PASSWORD` — its store/key password (alias must be `opentag`,
  or also set `OT_KEY_ALIAS`/`OT_KEY_PASSWORD` env vars in the workflow)

The workflow prefers the secret over the committed key. Switching keys
breaks upgrade-over-install for existing users — they uninstall once.

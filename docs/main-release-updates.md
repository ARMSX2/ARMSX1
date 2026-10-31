# Main branch and in-app updates

`main` is the source for release development. The original-engine CI checks run
on pushes to main. Cheat database and texture catalogue downloads use main too.
Existing per-game files and user-managed texture folders are preserved.

The GitHub app flavor checks published GitHub releases, not branch commits or
Actions artifacts. Publishing a tested main-branch APK requires the normal
`com.nanodata.armsx` package, the existing release signing certificate, a newer
Android version code and the own-engine libraries. Use a semantic release tag
such as `v0.3.20`; the updater also supports dated nightly release tags when the
user enables that channel. A commit or workflow artifact alone is not a release.
The Play flavor intentionally excludes the sideload updater.

Before handing a download to Android's installer, the updater checks package,
version code, signing certificate and engine library identity, as well as expected
download length and a GitHub SHA-256 asset digest when supplied. Installing an
update does not require uninstalling or clearing the app's saves/settings.

# M45 Software signed APT repository

Public archive: <https://m45software.com/apt/>. Suite: `stable`; component: `main`; architecture: `amd64` (including architecture-independent packages). Packages are currently built and tested on Ubuntu 24.04.

## Server layout

- `/var/www/m45software.com/apt`: public packages, indexes, signatures and public key. This directory is owned by `dist`; Nginx serves it through the existing HTTPS site.
- `/home/dist/.local/share/m45-apt/gnupg`: private signing key and revocation certificate. Both this directory and its parent have mode `0700`; private material is never copied into the web root or Git.
- `/home/dist/.local/share/m45-apt/signing-key.fingerprint`: archive key identity.
- `/home/dist/.local/share/m45-apt/publish-apt.py`: installed copy of the publisher from this project.

The dedicated RSA-3072 signing key has fingerprint `F8BD2FC443473EB71D87003BF8A7E75C58D7C0A3` and expires on **2028-11-08**. It is available for unattended signing to the `dist` account through the private GnuPG directory. Server access and system-backup permissions protect it.

## Automatic publication from GitHub Actions

Configured in `.github/workflows/publish-apt.yml`. Published GitHub releases (including the project's preview releases) deploy their matching `m45-drive-watcher_VERSION_amd64.deb` and `SHA256SUMS`. Drafts do not deploy. The workflow checks the checksum before upload; the server independently verifies size, checksum, package name, version and architecture before signing.

The build workflow calls the publisher directly after creating a tested release. This direct call is necessary because events created with GitHub's `GITHUB_TOKEN` do not trigger another release workflow. Manually published releases use the `release: published` event.

The repository Actions secret `M45_APT_DEPLOY_KEY` contains a dedicated SSH credential. Its public key has a `restrict`/forced-command entry in the server's `authorized_keys`, allowing only the fixed receiver at `/home/dist/.local/share/m45-apt/receive-apt-release.py`. It cannot run a shell, use SFTP/SCP, or forward connections. The receiver accepts this project's amd64 packages only, limits uploads to 64 MiB, and cleans up temporary uploads. CI cannot overwrite the receiver or publisher. The archive signing key stays on the server and is never uploaded to GitHub.

`packaging/apt/ssh_known_hosts` pins the already-verified server host key; the workflow requires strict host-key verification. If that key changes, verify it independently before updating the pin.

Use **Actions → Publish APT repository → Run workflow** with **check_only** enabled to test the restricted connection without publishing. To retry a published release, enter its tag (such as `v0.1.12`) and disable **check_only**. Reusing a version with different package bytes is rejected; increment the app version instead.

To rotate the Actions credential, create a fresh dedicated SSH key, add the same restricted entry for its public key, update `M45_APT_DEPLOY_KEY`, test the connection, and then remove the old public key. If that secret is lost, generate a replacement SSH key; do not replace the archive signing key. The receiver and signing state remain covered by the previously verified system-backup paths.

## Publish an update manually

Build and test the app as described in the README. Transfer the tested package and the current publisher to the server, then run the publisher there:

```sh
scp -P 5313 scripts/publish-apt.py build/m45-drive-watcher_VERSION_amd64.deb dist@m45software.com:/home/dist/.local/share/m45-apt/
ssh -p 5313 dist@m45software.com 'python3 /home/dist/.local/share/m45-apt/publish-apt.py --repository /var/www/m45software.com/apt --gnupghome /home/dist/.local/share/m45-apt/gnupg --signing-key "$(cat /home/dist/.local/share/m45-apt/signing-key.fingerprint)" /home/dist/.local/share/m45-apt/m45-drive-watcher_VERSION_amd64.deb'
```

Replace `VERSION` with the new package version. The publisher accepts `m45-*` packages for `amd64` or `all`. It refuses to replace an existing package version with different bytes. Every changed package must get a new version.

The publisher generates `Packages`, `Packages.gz`, `Release`, `Release.gpg` and `InRelease`. It publishes immutable SHA-256-addressed indexes before replacing `InRelease` atomically, retaining older indexes so clients can finish concurrent updates. Passing no package paths re-signs the current archive. The private key remains on the server.

## Public-key setup package

The public key is also tracked at `packaging/apt/m45-software.asc`. Rebuild the reproducible setup package with:

```sh
python3 scripts/build-apt-keyring.py --public-key packaging/apt/m45-software.asc --output build/m45-software-archive-keyring_1.0_all.deb
```

It contains only the public key and a `.sources` file using `Signed-By`. It has no maintainer scripts. Keep the first-download checksum in the README synchronized with the published setup package. A change to the trusted public key or repository configuration requires a new setup-package version; publish that through the existing signed archive before the old key expires.

## Backup and recovery

The server's full-system backup must include **both** `/var/www/m45software.com/apt` and `/home/dist/.local/share/m45-apt`, with no exclusion matching `.local`, `gnupg`, `private-keys-v1.d`, `openpgp-revocs.d` or the archive's `pool` and `dists` directories. Backup coverage was verified on **2026-11-09** against the server's actual system cron jobs and shared `/backup-lib.sh` implementation:

- Weekly: `/sysbak.sh`, using `/exclude.txt`, Sundays at 09:00 server time (UTC).
- Monthly: `/fullsysbak.sh`, using `/fullbakex.txt`, on the 1st at 07:00 UTC.
- Both archive `/`; the dynamic exclusions apply to selected `/var/lib` directories, not either archive path above.
- A GNU tar selection check using both real exclusion files confirmed that the public repository, private key, revocation certificate, and all their parent directories are included. Output was discarded to `/dev/null`; no key contents were printed or full backup started.

No exclusions needed changing. The new files will enter the next successful scheduled system backup; this check does not claim that the newly created key already exists in an off-server archive.

Restore the complete private state directory from the system backup, owned by `dist`, with the parent and GnuPG directory at mode `0700`. Confirm the expected fingerprint using `gpg --homedir /home/dist/.local/share/m45-apt/gnupg --list-secret-keys`. Restore the public archive at its original location. Re-run the publisher without package arguments to verify signing and refresh metadata. Do not generate a replacement key during recovery: existing clients trust the original key.

Before 2028-11-08, renew or rotate the signing key and distribute updated public-key metadata through a new setup-package version. Retain the revocation certificate in the protected system backup.

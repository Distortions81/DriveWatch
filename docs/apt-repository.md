# M45 Software signed APT repository

Public archive: <https://m45software.com/apt/>. Suite: `stable`; component: `main`; architecture: `amd64` (including architecture-independent packages). Packages are currently built and tested on Ubuntu 24.04.

## Server layout

- `/var/www/m45software.com/apt`: public packages, indexes, signatures and public key. This directory is owned by `dist`; Nginx serves it through the existing HTTPS site.
- `/home/dist/.local/share/m45-apt/gnupg`: private signing key and revocation certificate. Both this directory and its parent have mode `0700`; private material is never copied into the web root or Git.
- `/home/dist/.local/share/m45-apt/signing-key.fingerprint`: archive key identity.
- `/home/dist/.local/share/m45-apt/publish-apt.py`: installed copy of the publisher from this project.

The dedicated RSA-3072 signing key has fingerprint `F8BD2FC443473EB71D87003BF8A7E75C58D7C0A3` and expires on **2028-11-08**. It is available for unattended signing to the `dist` account through the private GnuPG directory. Server access and system-backup permissions protect it.

## Publish an update

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

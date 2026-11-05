# Releasing

How a build gets from the repository to a user's browser. **The workflows have never run on GitHub.** The scripts they call have all been run locally, and the section "What is verified" says exactly how far that goes.

## The idea

```
tag v0.2.0 ─┐                                   ┌─> gh-pages branch  ──>  https://<owner>.github.io/<repo>/   (the installer)
            ├─> ci ─> plan ─> firmware ─────────┤      versions.json, firmware/<board>/<channel>/<version>/…
nightly ────┘            └──> installer (ESP Web Tools) ─┘
                                                 └─> GitHub Release (stable only): images, .elf, SHA256SUMS
```

- **Stable** comes from a tag `vX.Y.Z` only. **Nightly** runs every night at 03:17 UTC (skipped when there are no new commits) or on demand from the Actions tab.
- Every release runs the full CI first (host tests under ASAN/UBSAN, site and release-tool tests, simulator, firmware build, secret scan). A red CI means no release.
- The installer page that is published is the page from the **same commit** as the firmware, so they are tested together.

## Cutting a release

1. `VERSION` holds the version **in development** (for example `0.2.0`). Make sure CI is green on `main`.
2. Tag exactly that version and push the tag:
   ```bash
   git tag v0.2.0 && git push origin v0.2.0
   ```
   `tools/version.py` refuses a tag that differs from `VERSION`, and the workflow refuses a tag that is not on `main`.
3. Watch the run. It builds, publishes to `gh-pages`, and creates the Release with the merged image, the `.elf` (to decode crash backtraces) and `SHA256SUMS`.
4. Bump `VERSION` to the next planned version (`0.3.0`) so nightlies sort correctly: `0.3.0-nightly.…` is newer than `0.2.0` and older than `0.3.0`.

Versions: `X.Y.Z` or `X.Y.Z-prerelease` (a tag with a dash, such as `v0.3.0-rc.1`, becomes a GitHub pre-release). At most 31 characters, because the firmware stores it in a 32-byte field. The same string is baked into the firmware (`project_version`, shown in Settings, About), the manifest and the index.

## Pulling a bad release

```bash
git checkout gh-pages
python3 -I tools/make_manifest.py --board amoled-175 --channel stable --version 0.2.0 --out . --remove
git commit -am "pull 0.2.0" && git push
```

The index is rewritten first, so the page stops offering it immediately; then its files are deleted. Also mark the GitHub Release as a pre-release or delete it.

## One-time GitHub setup

1. **Pages:** Settings, Pages, source = branch `gh-pages`, folder `/ (root)`. (The branch is created by the first publish.)
2. **Actions:** Settings, Actions, General, "Workflow permissions" may stay on read-only: the publish job asks for `contents: write` itself.
3. **Protect release tags:** a tag ruleset for `v*` so only maintainers can create them. Anyone who can push a `v*` tag can publish a release.
4. **Branch protection on `main`:** require the `ci` checks.
5. **Pin the actions.** The workflows use version tags (`actions/checkout@v4`, …). Pin them to full commit SHAs and let Dependabot update them; a moved tag is a supply-chain risk and could not be checked here.

No secrets are needed: publishing uses the built-in `GITHUB_TOKEN`.

## The scripts (all runnable locally)

| Script | Does | Tested by |
|---|---|---|
| `tools/version.py` | Decides the version; refuses a tag that disagrees with `VERSION`; nightly = `X.Y.Z-nightly.YYYYMMDD` | `tests/tools/test_release_tools.py` |
| `tools/make_manifest.py` | Adds a build (checks the image, writes the manifest, updates and prunes the index); `--remove` pulls one | `tests/tools/test_make_manifest.py`, `test_release_tools.py` |
| `tools/check_site.mjs` | Audits a site tree: index rules, files present, size and SHA-256, manifest consistent, erase prompt set, files world-readable, nothing escapes the site | `tests/tools/test_release_tools.py` |
| `tools/publish_site.sh` | Page files + vendor component + a build into a site tree, then the audit. Refuses a wrong image before touching anything | dry run below |
| `tools/vendor_esp_web_tools.sh` | Builds the installer component from a pinned commit | **not run** (needs npm) |

Dry run of the whole publish step with a real image (this is what the workflow does, minus the build):

```bash
tools/publish_site.sh --site /tmp/site --board amoled-175 --channel stable --version 0.1.0 \
    --merged dist/dzpanel-1.75-merged.bin --vendor <dir with install-button.js>
node tools/check_site.mjs /tmp/site --require-vendor
```

## What is verified

- **Run locally:** `version.py` (7 tests), `make_manifest.py` including `--remove` and file modes (7 tests), `check_site.mjs` (9 tests: a flipped byte, a missing manifest, a lost erase prompt, a version mismatch, unreadable files, a path escaping the site, missing page files, no index), `publish_site.sh` (stable publish, nightly publish, pull, audit, refusal of a bad image with nothing left behind).
- **Firmware version plumbing:** `firmware/CMakeLists.txt` takes `VERSION` or `DZPANEL_VERSION`, rejects a malformed string at configure time, and the value appears in `project_description.json`.
- **The workflow YAML** parses, and every script path it references exists.

## What is not verified

- **Nothing in `release.yml` has run on GitHub.** That includes the `workflow_call` reuse of `ci.yml`, the artifact hand-off between jobs, the `gh-pages` worktree handling, `gh release create`, and the permissions.
- The container tag `espressif/idf:v5.5.6` and the action versions.
- `tools/vendor_esp_web_tools.sh` (npm). If it fails, the release fails, which is the right outcome: a page whose install button cannot load must not be published.
- **No release image has been flashed.** The images are only known to compile and merge.
- Nightly scheduling skips on "no commits in 25 hours"; the first night will show whether that rule is right.
- Signing: images are protected by SHA-256 against corruption, **not** against a hostile host. Signed OTA is a later stage (`DESIGN.md` section 10).

## Local bring-up package vs a release

`dist/` (git-ignored) is a hand-made package for the first day on a board: Waveshare's factory image, their demo built here, and our image. See `docs/bringup.md`. A release is what the workflow publishes.

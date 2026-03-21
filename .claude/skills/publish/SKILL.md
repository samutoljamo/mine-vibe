---
name: publish
description: Use when releasing a new version of the minecraft project — creates a version tag that triggers the release workflow to build and publish Linux and Windows binaries on GitHub.
---

# Publish (minecraft release)

Tagging `v*` triggers `.github/workflows/release.yml`, which builds both platforms and creates a GitHub Release with two binaries attached.

## Steps

1. **Verify state**
   ```bash
   git status          # must be clean
   git log --oneline -5
   ```
   Confirm you're on `main` and CI is green.

2. **Pick a version** — follow semver: `v<major>.<minor>.<patch>`
   - Patch: bug fixes
   - Minor: new gameplay features
   - Major: breaking save/protocol changes

3. **Tag and push**
   ```bash
   git tag v1.2.3
   git push origin v1.2.3
   ```

4. **Monitor the release workflow**
   ```bash
   gh run list --workflow=release.yml
   gh run watch
   ```

5. **Verify the release**
   ```bash
   gh release view v1.2.3
   ```

## What gets published

| Platform | File | Notes |
|----------|------|-------|
| Linux | `minecraft-linux-x86_64` | Requires Vulkan drivers + X11 |
| Windows | `minecraft-windows-x86_64.exe` | Requires Vulkan drivers; MSVC runtime statically linked |

Assets and shaders are embedded — both binaries are self-contained (no extra files to distribute).

## Deleting a bad tag

```bash
git tag -d v1.2.3
git push origin :refs/tags/v1.2.3
```

Then delete the GitHub Release if it was created: `gh release delete v1.2.3`.

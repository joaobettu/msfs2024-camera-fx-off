## What this changes

<!-- What the pull request does and why. Link the issue it fixes, if any ("Fixes #12"). -->

## How it was tested

<!-- Check what you ran. If you tested in the sim, say with which settings (upscaling, resolution, HDR) and what you looked at. -->

- [ ] `scripts\build.ps1 -Tests` builds without warnings
- [ ] `scripts\test.ps1` passes (WARP)
- [ ] `scripts\test.ps1 -Gpu` passes (with the sim closed)
- [ ] Tested in the sim

## Checklist

- [ ] Text the user sees (overlay, log, README) is in English
- [ ] The README is updated if the behavior or the settings changed
- [ ] Nothing from the game (shaders, `dxcompiler.dll`, packages) and nothing from `build\` is committed
- [ ] No machine-specific paths or personal data in code, docs or images

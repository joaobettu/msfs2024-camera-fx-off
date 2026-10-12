# Contributing

Thanks for helping with MSFS 2024 Camera FX Off. The add-on is in beta, so bug reports and feedback from different setups are the most useful contribution right now.

## Reporting a problem

Open an issue with the **Bug report** form. Search the existing issues first, and keep to one problem per issue.

What helps the most:

- **The log**, `msfs2024-camera-fx-off.log`, from the game folder.
  - The add-on rewrites it every time the sim starts, so take it right after the problem.
- **A one-frame trace** for rendering problems: press Ctrl+Shift+F7 while the problem is on screen, then attach `msfs2024-camera-fx-off.trace.txt`.
- **Your setup:**
  - the sim version (Steam or Microsoft Store);
  - the ReShade version;
  - upscaling (DLSS, FSR, TAA, frame generation);
  - resolution and HDR;
  - the other ReShade add-ons or effects you use.
- **Screenshots** of the same view with the add-on's option on and off.

## Security problems

Don't open a public issue. Report the problem privately with [Report a vulnerability](https://github.com/joaobettu/msfs2024-camera-fx-off/security/advisories/new) instead.

## Suggesting a feature

Open an issue with the **Feature request** form. Say what bothers you in the sim and how you would like it to look; a screenshot often says more than a description.

## After a sim update

Sim updates can change the game's shaders, and two parts of the add-on depend on them.

- **Bloom.** If the shader package changes, the add-on leaves bloom on and says so in the log and the overlay. To update it:
  1. Run `tools\gen_twins.ps1` to write a new `build\msfs2024-camera-fx-off.twins`. It refuses to write the file if any pair is ambiguous.
  2. Update the counts that `tests\classify_test.cpp` expects for the new package.
- **Lens flare parts.** The glow, star rays and ghosts are told apart by the digests of their shaders.
  - For shaders it doesn't know, the add-on goes by draw order, and the log says so: "has shaders this version does not know", followed by the VS and PS digests.
  - Check in the sim which part each of those draws by toggling the **Keep** options.
  - Then add their digests to `known_flare_part` in `src/classify.hpp` and to the table in `tests/classify_test.cpp`.

## Contributing code

### Building and testing

You need the Visual Studio 2022 Build Tools (C++ x64) and the Windows 11 SDK. From the repository folder:

```
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Tests
powershell -ExecutionPolicy Bypass -File tools\gen_twins.ps1
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

The scripts find MSFS 2024 in your Steam libraries. For other installs, pass `-GamePath`.

- `scripts\test.ps1` runs on WARP, the CPU-only rasterizer, so it is safe while the sim is running.
- `scripts\test.ps1 -Gpu` runs the same tests on the graphics card. Run it with the sim closed.

The README describes what each test covers.

### Guidelines

- **Keep the hot paths cheap.** The pipeline bind, draw and render target events run thousands of times per frame, from several threads. Don't take locks, allocate memory or write to the log on those paths, except once per pipeline or lens flare element.
- **Never distribute game files.** That covers shader bytecode, `dxcompiler.dll` and packages. The repository and the releases only hold digests, offsets and sizes taken from the shader package. `build\` is ignored by git; keep it that way.
- **Write everything the user sees in English:** the overlay, the log and the README.
- **Follow the surrounding code.** It is C++20 with tabs, and comments explain why rather than what.
- **Add or update a test** when you change behavior. The fixture draws stand-ins of the game's shaders, so most changes can be checked without the sim.
- **Keep machine-specific paths and personal data** out of code, docs and images.

### Pull requests

- Keep each pull request to one topic.
- Fill in the template: what the change does, why, and how you tested it.
- If the change affects what is drawn, test it in the sim and say with which settings.

By contributing, you agree that your contribution is licensed under the [MIT License](LICENSE), like the rest of the project.

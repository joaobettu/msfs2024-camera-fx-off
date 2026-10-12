# MSFS 2024 Camera FX Off

By **PdPanoo**

A ReShade add-on that removes the "filmed with a camera" look from Microsoft Flight Simulator 2024:

- **Lens flare:** the ghosts and lens glare the game draws when the sun is on screen go away. Each element of the flare can be kept from the overlay, for example the glow around the sun.
- **Bloom and lens dirt:** tonemapping uses the game's own permutation **without bloom**, the same one the engine would use with bloom turned off. Lens dirt only exists together with bloom, so it goes away too.

City, airport and aircraft lights are left alone.

## Requirements

- ReShade 6.8 or newer, the version **with full add-on support**.
- Microsoft Flight Simulator 2024 (DirectX 12). Tested with the Steam version.
- Works alongside other add-ons, such as *Glass Cockpit DLSS Fix*.

## Installation (with the sim closed)

Get the two files from the [latest release](https://github.com/joaobettu/msfs2024-camera-fx-off/releases), or from `build\` if you build the add-on yourself. Copy them into the game folder, the one that holds `FlightSimulator2024.exe` and ReShade's `dxgi.dll` (for the Steam version, `steamapps\common\Limitless`):

- `msfs2024-camera-fx-off.addon64`
- `msfs2024-camera-fx-off.twins` (the shader pairs for the bloom removal; without it, only the lens flare removal works)

To uninstall, delete those files and the ones the add-on creates next to them (`.ini`, `.log`, `.trace.txt`).

## Usage

| Shortcut | Action |
|---|---|
| Ctrl+Shift+F5 | turns the lens flare removal on or off |
| Ctrl+Shift+F6 | turns the bloom removal on or off |
| Ctrl+Shift+F7 | records a one-frame trace to `msfs2024-camera-fx-off.trace.txt` |

The same options are in the ReShade overlay, **Add-ons** tab, along with counters per second.

### Keeping part of the lens flare

The lens flare is made of several elements. Under the lens flare option, the overlay lists every element the game has drawn so far. Look at the sun with the overlay open and they show up, each one with a **Keep** checkbox and the number of quads it draws per second.

A kept element is still drawn while the rest of the lens flare is removed. Check them one at a time to find the one you want, such as the glow around the sun. Your choice is saved and applies to later flights too.

- Elements are named after their shaders (`VS xxxxxxxx, PS yyyyyyyy`). A kept element stays kept as long as a sim update does not change those shaders.
- Each element is one pair of shaders. Some elements draw several quads per frame, and those quads are kept or removed together.

Bloom also draws a glow around the sun. If that glow is all you miss, turning the bloom removal off brings it back, along with the lens dirt.

### Settings file

Your choices are saved in `msfs2024-camera-fx-off.ini`, next to the add-on, in the `[settings]` section. The add-on reads it when the sim starts and rewrites it whenever you change an option in game, so edit it by hand only with the sim closed.

| Key | Default | Meaning |
|---|---|---|
| `sun_flare_off` | `1` | remove the lens flare |
| `flare_keep` | (empty) | lens flare elements to keep, as `vvvvvvvv:pppppppp` (vertex and pixel shader), separated by commas |
| `bloom_off` | `1` | remove bloom and lens dirt |
| `key_flare`, `key_bloom`, `key_trace` | `116`, `117`, `118` (F5, F6, F7) | shortcut keys, pressed with Ctrl+Shift, as Windows virtual-key codes |
| `light_glow_off`, `light_point_off` | `0` | diagnostics, see below |
| `pak_path` | (empty) | full path of the game's `SHADERSPAK.*.SM6.PC.PAK`, if it is not in the `Shaders` folder next to the game executable |

The log, `msfs2024-camera-fx-off.log`, is rewritten every time the sim starts. It shows:

- the tonemaps that got their no-bloom twin;
- the lens flare elements seen;
- the pipelines whose quads were removed as lens flare.

The diagnostic options ("Hide the light glow", "Hide the light points") only show in game what each class of light shader draws. They **hide lights** and are off by default.

## After a sim update

The lens flare is recognized from the context of each draw, not by shader hash, so its removal should keep working. Kept elements, though, are named after their shaders. If an update changes those shaders, pick the elements to keep again.

The bloom removal depends on a table of pairs made for the current shader package. If the sim updates, the log and the overlay say so, and bloom simply stays on. To regenerate the table, run this from the source folder (pass `-GamePath` if the game is not in `D:\SteamLibrary\steamapps\common\Limitless`):

```
powershell -ExecutionPolicy Bypass -File tools\gen_twins.ps1 -GamePath "<game folder>"
```

Then copy the new `build\msfs2024-camera-fx-off.twins` into the game folder. The tool only reads the game files, and it refuses to write the table if any pair is ambiguous.

## How it works

Everything is done through ReShade events, with no hooks of its own.

- **Lens flare:** after upscaling (DLSS/FSR/TAA) and before post-processing, the engine has a `do_flare` pass.
  - Each element of the flare is drawn there as a quad (a 4-vertex `draw`) with the generic material shaders (`vh_base`/`ph_base`).
  - The target is the HDR image (R11G11B10), with no depth bound.
  - The add-on skips exactly those draws, except for the elements set to keep.
  - The same shaders draw transparent geometry in the main pass, but always with depth bound, so those draws are left alone.
- **Bloom:**
  - Of the 770 `ph_tonemapping` permutations in the `SHADERSPAK`, 384 have bloom. All of them, and only them, bind the lens dirt texture at `t8`.
  - Each one has a no-bloom twin: the same resources minus `t8`/`s8`, and the same code minus the bloom block.
  - `tools\gen_twins.ps1` pairs them offline, with a sequence diff of the DXIL disassembled by the game's own `dxcompiler.dll`.
  - When the game creates a tonemap with bloom, the add-on reads the twin from the package, checks its digest, and creates an identical pipeline with it. Each time the game binds the original, the add-on binds the twin.
- **Lights:** the glow of each distant light is a `vh_billboard_lights` sprite with a `FlareUV` output. The star rays are part of that same sprite, so skipping the draw would erase the whole light. That is why the add-on leaves it alone.
- **Cost per frame:**
  - a lock-free table lookup on every pipeline bind;
  - a format lookup on every render target bind without depth;
  - one test per draw.
- **One-time cost:** the first time each tonemap with bloom is created, the driver compiles its twin. That takes about 65 ms, once.

## Known limitations

- The sim compiles some tonemap permutations at run time. Those are not in the `SHADERSPAK`, so they have no twin and keep their bloom. The log shows "no pair for tonemapping variant" for them. In the main view tested, every tonemap in use had a twin.
- The star rays on lights remain, because they are part of the sprite that draws the light itself.

## Building and testing

You need the Visual Studio 2022 Build Tools (C++ x64) and the Windows 11 SDK.

```
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Tests
powershell -ExecutionPolicy Bypass -File tools\gen_twins.ps1
powershell -ExecutionPolicy Bypass -File scripts\test.ps1
```

The scripts default to the game in `D:\SteamLibrary\steamapps\common\Limitless`. If yours is elsewhere, pass:

- `-GamePath` to `gen_twins.ps1` and `test.ps1`;
- `-ReShadeDll` (the path of ReShade's `dxgi.dll`) to `build.ps1`.

What the tests cover:

- `classify_test` runs against the real `SHADERSPAK`, read-only.
  - It checks the counts: 4 light glow shaders, 2 light point shaders and 384 tonemaps with bloom. These are the counts for the shader package the add-on was tested with (`SHADERSPAK.d5d2118d.SM6.PC.PAK`); a sim update may change them.
  - It validates every pair in the table.
  - It fuzzes the parsers with the end of the buffer right against an inaccessible page, so any out-of-bounds read breaks the test.
- `fixture` loads a copy of ReShade's `dxgi.dll` with the add-on, on a D3D12 device on WARP (CPU only). It draws stand-ins for the MSFS shaders, with the same names and signatures, and checks the pixels:
  - the lens flare quad on an HDR target without depth is skipped;
  - the quad of a lens flare element set to keep is drawn;
  - the same quad with depth bound, and a 3-vertex draw, are kept;
  - the lights are kept;
  - the tonemap is swapped for its twin.
- An extra `fixture` run builds pipelines from the game's own light shaders and bloom tonemap shader, taken from its package. It checks that the add-on recognizes them and creates the real no-bloom twin. These pipelines are only compiled, never drawn.
- `scripts\test.ps1 -Gpu` runs the `fixture` on the graphics card. While the sim is running, it only does so with `-Force`.

## Credits and license

- Author: PdPanoo. Bug reports and suggestions are welcome in the [issues](https://github.com/joaobettu/msfs2024-camera-fx-off/issues).
- License: MIT, see [LICENSE](LICENSE).
- Third-party code, under its own licenses:
  - ReShade 6.8.0 add-on SDK headers (`third_party/reshade`): BSD-3-Clause OR MIT, © Patrick Mours. See `third_party/reshade/LICENSE.txt`.
  - Dear ImGui 1.92.5 (`third_party/imgui`): MIT, © Omar Cornut. See `third_party/imgui/LICENSE.txt`.

This project is not affiliated with or endorsed by Microsoft, Asobo Studio or the ReShade project.

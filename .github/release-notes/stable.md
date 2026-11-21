> [!IMPORTANT]
> ## Please read first
> - **Kirby Air Riders:** races can still hang the GPU. Please don't play it on Orchid yet.
> - **New user folder:** new installs keep their settings, saves and cache in `orchid` folders (`~/.local/share/orchid`, `~/.config/orchid`, `~/.cache/orchid` on Linux, `%APPDATA%\orchid` on Windows). If you already use Orchid, nothing changes: your existing `eden` folders stay in use and nothing is moved. To switch, rename them to `orchid`; the log says how ([#154](https://github.com/MythosOfs/Orchid/issues/154)).
> - Found a problem? Use **Help > Report an Issue on GitHub** inside Orchid. It fills in your build, system, GPU, driver and recent game log for you.

# Orchid 0.12.0

HDR output, a 4K option for Yoshi, smoother Donkey Kong Bananza and Star Fox, and many fixes for crashes, controllers and game files.

## New

- **HDR output:** Switch 2 games with an HDR mode can now show it in HDR. Turn it on in **Graphics > HDR output** (Off, Auto, On; Off by default, Vulkan only). HDR10 and scRGB are supported. Auto works on Windows; on Linux use On and run Orchid as a Wayland client. While a frame is in HDR, anti-aliasing, post-processing, FSR upscaling and frame generation are paused ([#178](https://github.com/MythosOfs/Orchid/issues/178)).
- **Yoshi and the Mysterious Book:** a built-in 4K game patch makes the game sharp instead of blurry. Turn on **Internal resolution 4K (game patch)** in the game's properties. Off by default, for game version 1.0.2; an installed resolution mod takes precedence. Patch by Walnutoast830 ([#144](https://github.com/MythosOfs/Orchid/issues/144)).
- **NXFormat game archives** (`.nx`) can be opened and show up in the game list ([#166](https://github.com/MythosOfs/Orchid/issues/166)).

## What's better in your games

- **Donkey Kong Bananza:** breaking terrain runs smoother (in our test scene about 50 → 56 FPS) ([#182](https://github.com/MythosOfs/Orchid/issues/182)), and the game no longer drops to 30 FPS when the CPU clock is set to Boost ([#180](https://github.com/MythosOfs/Orchid/issues/180)).
- **Star Fox:** runs at about 58 FPS again instead of 39 when the CPU clock is set to Boost ([#180](https://github.com/MythosOfs/Orchid/issues/180)).
- **Yoshi and the Mysterious Book:** loading a save no longer ends the game with error 2162-0001 ([#181](https://github.com/MythosOfs/Orchid/issues/181)).
- **Kirby and the Forgotten Land:** the fog in the first Star-Crossed World stage is back on NVIDIA RTX 20 series and newer ([#179](https://github.com/MythosOfs/Orchid/issues/179)).
- **Super Mario Bros. Wonder:** no longer freezes the PC on 12 GB graphics cards when video memory runs out ([#123](https://github.com/MythosOfs/Orchid/issues/123)).
- **Hyrule Warriors: Age of Imprisonment:** no more tearing line near the top of the screen. Game presets no longer change your VSync mode ([#167](https://github.com/MythosOfs/Orchid/issues/167)).
- **Hyrule Warriors: Age of Imprisonment:** less GPU-thread work in fights when textures are uploaded ([#143](https://github.com/MythosOfs/Orchid/issues/143)).
- **Game videos:** when the GPU video decoder can't play a video, Orchid now plays it on the CPU instead of showing nothing.
- **Standalone updates (`.nup`)** that carry the Switch 2 program start that program instead of the Switch 1 one ([#153](https://github.com/MythosOfs/Orchid/issues/153)).

## Controllers

- A Pro Controller with vibration on no longer stops responding after a while ([#55](https://github.com/MythosOfs/Orchid/issues/55)).
- Vibration queries for a controller Orchid doesn't know no longer make a game abort ([#111](https://github.com/MythosOfs/Orchid/issues/111)).
- Linux: when a Bluetooth controller connects but sends no input because Orchid can't open its device, the input settings say so and show the udev rule that fixes it.
- Tab can be bound to a button.

## Orchid itself

- Stopping a game gives its memory back (about 3 GB after Donkey Kong Bananza).
- Windows: Orchid starts from a folder with accented or other non-English letters in its path (PGO build) ([#116](https://github.com/MythosOfs/Orchid/issues/116)).
- Windows: installed game files are no longer locked against a second read ([#113](https://github.com/MythosOfs/Orchid/issues/113)), and the game log opens again on a second game start.
- Settings and game properties scroll when they don't fit on the screen.
- Missing keys are named clearly: the error points to **Tools > Install Decryption Keys** or says which title key is missing, instead of "bad NCA header, redump".
- Loose dumps get the same name in the game list and the window title.
- **Dump RomFS** works on a loose `.nup` game.
- NVIDIA: a wrong driver-version check no longer turns off a Vulkan feature, so fewer shaders are compiled while playing.
- Stopping and restarting a game quickly no longer acts on the wrong session.
- The shader cache is saved safely, so an interrupted exit can't damage it.
- Damaged keys files, cheats, NCZ files and game metadata are rejected instead of crashing Orchid.
- The multiplayer ban lists are saved correctly.

## Downloads

The release files are also on GitLab: https://gitlab.com/MythosOfs/Orchid/-/releases

## Thanks

Many fixes in this release come from other Eglantine forks: zortos293/Eglantine, Lichii and its forks, astraea-emu/nx2 and Eglantine Community. Thanks to Zortos, Daniel Jesús Martín Flores, Matt Artist, 999sian, Carlos Lopez, 101Leafy, SmileyTatsu, Delio Azeredo, Luke Evanson, RomainROCH, Amix and everyone else whose work went in, to Walnutoast830 for the Yoshi patch, and to everyone who sent logs and tested builds.

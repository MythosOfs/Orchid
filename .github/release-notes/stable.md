> [!IMPORTANT]
> ## Please read first
> - **Kirby Air Riders:** races can still hang the GPU. Please don't play it on Orchid yet.
> - **Mario Tennis Fever:** the experimental frame rate patch is gone. CPU opponents kept missing the ball with it, so the game now always runs at its own 60 FPS. An old setting for it is simply ignored.
> - Found a problem? Use **Help > Report an Issue on GitHub** inside Orchid. It fills in your build, system, GPU, driver and recent game log for you.

# Orchid 0.11

A smaller release focused on the problems you reported after 0.10: memory that kept growing, games that got stuck while loading, and a few graphics glitches.

## What's better in your games

- **Kirby and the Forgotten Land (Switch 2 Edition):** the slow memory growth is fixed. The game no longer freezes or crashes after a while, also on 8 GB graphics cards ([#140](https://github.com/MythosOfs/Orchid/issues/140)).
- **Super Mario Bros. Wonder:** graphics memory no longer piles up when the game lowers its resolution under load. On 8 GB cards this could drop a level to about 42 fps for minutes. Logs are also much shorter now.
- **Donkey Kong Bananza:** the map no longer has lavender stripes and flat surfaces over it ([#53](https://github.com/MythosOfs/Orchid/issues/53)).
- **Yoshi and the Mysterious Book:** fixed a cause of the game getting stuck when a level loads. We couldn't reproduce the hang ourselves, so please report it if it still happens.
- **Windows:** games no longer abort on the loading screen when Orchid sits in a folder with a long path (for example a long folder name). Very long paths still need Windows' "long paths" system setting.
- **Kirby and the Forgotten Land (Switch 2 Edition) on NVIDIA:** the flat cyan band over the lower screen in the opening forest is gone with Vulkan. On NVIDIA the fog of that one scene effect is switched off to avoid it; other GPUs keep it ([#164](https://github.com/MythosOfs/Orchid/issues/164), workaround by Nash21979).
- **Switch 2 games** that ask for system fonts, time zones and other system data by their Switch 2 id now get them instead of an empty file.

## New per-game options

All of these are off by default. Find them in a game's **Properties > System**; they take effect the next time you start the game.

- **Donkey Kong Bananza: Dynamic frame rate (variable refresh rate display).** The game uses its own frame rate control between 40 and 60 fps instead of dropping straight to 30 when it can't hold 60.
- **Hyrule Warriors: Age of Imprisonment: Keep game speed below 60 FPS.** The game ran in slow motion when your PC couldn't hold 60 fps. With this on it switches to 30 fps at full speed instead.
- **Pokémon Pokopia: Frame rate (FPS).** The game runs correctly at other frame rates, so you can pick a higher one.

## Downloads

The release files are also on GitLab: https://gitlab.com/MythosOfs/Orchid/-/releases

## Thanks

Thanks to everyone who sent logs and tested builds, and to Nash21979 for the Kirby fog workaround.

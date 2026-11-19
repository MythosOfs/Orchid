> [!IMPORTANT]
> ## Please read first
> - **Kirby Air Riders:** races can still hang the GPU. Please don't play it on Orchid yet.
> - **Mario Tennis Fever:** the experimental frame rate patch is gone. CPU opponents kept missing the ball with it, so the game now always runs at its own 60 FPS. An old setting for it is simply ignored.
> - Found a problem? Use **Help > Report an Issue on GitHub** inside Orchid. It fills in your build, system, GPU, driver and recent game log for you.

# Orchid 0.11.1

A small update with fixes for Pokémon Pokopia and Star Fox, and fewer crashes from damaged files.

## What's better in your games

- **Pokémon Pokopia:** the game no longer gets stuck on a black screen after its first frame. This happened on a first start, or the first start after an update, with the Standard preset ([#113](https://github.com/MythosOfs/Orchid/issues/113)).
- **Pokémon Pokopia:** saved photos no longer have black bands or pieces of older photos in them ([#58](https://github.com/MythosOfs/Orchid/issues/58)).
- **Star Fox:** a controller whose type the game doesn't support (for example a GameCube pad) no longer keeps disconnecting and reconnecting ([#55](https://github.com/MythosOfs/Orchid/issues/55)).

## Fewer crashes from damaged files

A damaged shader cache, a broken game dump, a malformed `.pchtxt` mod or an add-on folder with an odd version name could crash Orchid or hang the boot. Orchid now skips such data instead.

## Downloads

The release files are also on GitLab: https://gitlab.com/MythosOfs/Orchid/-/releases

## Thanks

Thanks to everyone who sent logs and tested builds.

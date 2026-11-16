# Nestopia for the Cardputer Zero

A small front end for the Nestopia emulator core: the picture goes to the Zero's LCD, the keys come from the built-in keyboard, and the sound plays through SDL2 (PipeWire).
It runs at about 180 frames per second on the Zero with the games tested (the console needs 60). No games are included: put your own ROM files (`.nes`) in `~/roms`.

## Use

Open Nestopia in the launcher: a list of `~/roms/*.nes` appears (up / down, Enter). Hold `Esc` for a second in a game to go back to the list, and in the list to quit.
From a shell: `nestopia-cz game.nes`.

| Keys | Pad |
| --- | --- |
| `Z` `X` `F` `C`, or the arrow keys | left, down, up, right |
| `A` or `K` | A |
| `S` or `J` | B |
| `Enter` | Start |
| `Backspace` | Select |
| `1` / `2` | save / load the state (`<rom>.state`) |
| hold `Esc` | back to the list, or quit |

Options: `--scale tv|fit|crop` (`tv`, the default, is 4:3 at 227x170, smoothed; `fit` is square pixels at 181x170; `crop` is the middle 256x170, not scaled), `--no-sound`,
`--headless N [--ppm file]` (run N frames without a screen and print the speed).

## Build

The Nestopia source is not copied here: `third_party/PINS` names the upstream commit and `third_party/fetch.sh` fetches it. The core builds with plain `g++`; the front end needs SDL2 for sound.

```sh
sh third_party/fetch.sh
sh build.sh            # on arm64 Debian 13 (the Zero or a container) -> out/nestopia-cz
```

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Nestopia is GPL-2.0-or-later; the other components are listed in [THIRD_PARTY.md](THIRD_PARTY.md).

Leo Kuroshita ([@kurogedelic](https://github.com/kurogedelic))

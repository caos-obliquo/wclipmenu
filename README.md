# wclipmenu

wmenu-based clipboard manager for Wayland. Picks from kaprica history — text or
images with thumbnail previews. C, suckless-style: one small on-demand binary,
no daemon, no polling, no config files.

## Synopsis

	wclipmenu
	wclipmenu list [-n count]
	wclipmenu image [-n count]
	wclipmenu copy <id>
	wclipmenu -h

## Description

`wclipmenu` is a thin glue layer between the kaprica clipboard daemon
(`kapd`/`kapc`) and a wmenu-based picker. It runs only when you press the
keybind, reads the kaprica history through the `kapc` CLI, shows a picker, and
exits after the selection is copied back to the clipboard. There is no resident
process.

With no arguments it picks from the text history (`text/plain`). `image` picks
from the image history (`image/png`) with PNG thumbnails rendered through the
custom [wmenu-caos](https://github.com/caos-obliquo/wmenu-caos) fork. `list` prints snippets to stdout and `copy <id>`
copies one entry — both are debug/CLI paths.

## Options

	-h, --help       print usage and exit
	list [-n count]  print `count` newest snippet lines to stdout (default 100)
	image [-n count] pick from the image history with thumbnail previews
	copy <id>        copy history entry `id` back to the clipboard

`count` is clamped to 1..10000 (kapd keeps up to 10000 entries).

## Environment

	WCLIPMENU_WMENU  path to the wmenu binary (default: `wmenu` from PATH)
	WCLIPMENU_LIMIT  max entries fetched from kaprica (default: 100)
	WCLIPMENU_DB     kaprica db file passed to kapc as -D (default: kaprica's)

The `WCLIPMENU_*` overrides exist for testing and are not needed for normal use.

## How it works

- Text picker: `kapc search -t text/plain -L -l <n>`, feed the `<id>\t<snippet>`
  lines to `wmenu -l 15`, then `kapc copy -i <id>`.
- Image picker: `kapc search -t image/png -L -l <n>`. Each entry's thumbnail is
  rendered with `magick -resize 160x160` to `/tmp/wclipmenu-thumb-<id>.png`
  (cached on disk, rendered in parallel child processes), then fed to the picker
  as `[img:<path>]<snippet>` lines — the [wmenu-caos](https://github.com/caos-obliquo/wmenu-caos) fork renders the PNGs.
  The image picker shows 5 entries per page (`-l 5`); arrow keys page through
  all results.
- Screenshots taken with Super+S / Super+Shift+S (dwl-screenshot) show up in the
  image picker on their own, because `kapd` observes the `wl-copy` selection.

## Requirements

- A Wayland session (dwl or any compositor)
- [kaprica](https://github.com/ArtsyMacaw/kaprica) installed:
  `kapd` daemon + `kapc` at /usr/local/bin/kapc
- [wmenu-caos](https://github.com/caos-obliquo/wmenu-caos) as the picker
  (needed for image thumbnails; plain `wmenu` works for text)
- ImageMagick (`magick`) for thumbnail rendering

## Build and install

	make
	sudo make install          # installs to /usr/local/bin
	make PREFIX=$HOME/.local install

Builds with `-std=c11 -Wall -Wextra -O2`, zero warnings required. There is no
`make test` shortcut from the repo root — see Tests below.

## Usage

Keybinds in dwl config:

	Super+P        text picker
	Super+Shift+P  image picker

Select with arrow keys, confirm with Enter (copies to clipboard), Escape
cancels. Typing filters the list; case-insensitive matching is on.

## Tests

	cd tests && make clean && make && make test

Requires a live Wayland session, kaprica installed, and **no system kapd
running** (kapd is a singleton). All 6 suites (daemon, text, search, reverse,
image, picker) run against an isolated db under `/tmp`, so the real history db
is never touched.

## Development

Feature branch → PR → CI green → merge to main. No direct pushes to main.
CI (`.github/workflows/ci.yml`) builds with `make` on build-essential for every
pull request and push to main; `make test` is not part of CI because it needs a
live Wayland session.

## Credits / inspiration

- [wmenu-caos](https://github.com/caos-obliquo/wmenu-caos): wmenu fork with
  `[img:]` PNG thumbnail rendering, used as the picker UI
- [wmenu](https://sr.ht/~adnano/wmenu/): upstream picker (dmenu-style,
  Wayland-native)
- [kaprica](https://github.com/ArtsyMacaw/kaprica): clipboard daemon + CLI
  backend
- [dwl](https://codeberg.org/dwl/dwl): the window manager / keybind host
- [wl-clipboard](https://github.com/bugaevc/wl-clipboard): Wayland clipboard
  utilities
- [clipmenu](https://github.com/cdown/clipmenu): the dmenu-based original this
  is modeled on

wclipmenu is a thin glue layer standing on their work.

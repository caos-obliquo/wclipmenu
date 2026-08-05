# wclipmenu

wmenu-based clipboard manager for Wayland. Pick from kaprica history: text or images
with thumbnail previews. C, suckless-style.

## Design

- Zero footprint: an on-demand binary. It runs only when you press the keybind, reads
  the kaprica history through `kapc`, shows a `wmenu` picker, and exits after the
  selection is copied back to the clipboard. No resident process, no polling, no bloat.
- Backend is the kaprica CLI only (`kapd` daemon + `kapc` client), no kapg, no GUI.
- Text picker: `kapc search -t text/plain -L -l 100 | wmenu -l 15` -> `kapc copy -i <id>`.
- Image picker: `kapc search -t image/png -L -l 100`. Each entry's thumbnail is rendered
  with `magick -resize 160x160` to /tmp/wclipmenu-thumb-<id>.png (cached on disk,
  rendered in parallel forks), then fed to the picker as `[img:<path>]<snippet>` lines.
  The selection is copied back via `kapc copy -i <id>`.
- Picker UI: the custom [wmenu-dwlb] fork renders the PNG thumbnails (128px thumbnail
  in a 160px row). The image picker shows 5 entries per page (`-l 5`), the text picker
  15 lines (`-l 15`); arrow keys page through all results (kapd keeps up to 10000
  history entries).
- Keybinds (dwl): Super+P text picker, Super+Shift+P image picker. Screenshots taken
  with Super+S / Super+Shift+S (dwl-screenshot) show up in the image picker on their
  own, because kapd observes the `wl-copy` selection.

## Status

Complete. The `wclipmenu` picker binary (text + image picking via wmenu) and the kaprica
CLI contract test harness in `tests/` (6 C suites: daemon, text, search, reverse, image,
picker) are both implemented. See `tests/docs/contract.md` and `tests/docs/run.md` for the
harness. The binary is installed at /usr/local/bin/wclipmenu, keybinds wired into dwl.

## Tests

```
cd tests && make clean && make && make test
```

Requires a live Wayland session, kaprica installed (/usr/local/bin/kapd + kapc), and
no system kapd running (kapd is a singleton). All suites use an isolated db under
/tmp, so the real history db is never touched.

## Development

Feature branch -> PR -> CI green -> merge to main. No direct pushes. CI is GitHub
Actions (`.github/workflows/ci.yml`): builds with `make` on build-essential, run for
every pull request and push to main. `make test` needs a live Wayland session and an
isolated kapd, so stop the system kapd first (singleton).

## Credits / inspiration

- [wmenu-dwlb](https://github.com/caos-obliquo/wmenu-dwlb): the custom wmenu fork with
  `[img:]` PNG thumbnail rendering used as the picker UI
- [wmenu](https://sr.ht/~adnano/wmenu/): the upstream picker (dmenu-style, Wayland-native)
- [kaprica](https://github.com/ArtsyMacaw/kaprica): clipboard daemon + CLI backend
- [cclip](https://github.com/erebe/cclip): Rust Wayland clipboard manager (concepts)
- [dwl](https://codeberg.org/dwl/dwl): the window manager / keybind host
- [wl-clipboard](https://github.com/bugaevc/wl-clipboard): Wayland clipboard utilities
- [clipmenu](https://github.com/cdown/clipmenu): the dmenu-based original this is modeled on

Special thanks to all of the above. wclipmenu is a thin glue layer standing on their work.

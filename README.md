# wclipmenu

wmenu-based clipboard manager for Wayland. Pick from kaprica history: text or images
with sixel previews. C, suckless-style.

## Design

- Zero footprint: an on-demand binary. It runs only when you press the keybind, reads
  the kaprica history through `kapc`, shows a `wmenu` picker, and exits after the
  selection is copied back to the clipboard. No resident process, no polling, no bloat.
- Backend is the kaprica CLI only (`kapd` daemon + `kapc` client) — no kapg, no GUI.
- Text picker: `kapc search -t text/plain -L -l 100 | wmenu` -> `kapc copy -i <id>`.
- Image picker: `kapc search -t image/png -L -l 100` with `-x` sixel previews ->
  `kapc copy -i <id>`.
- Keybinds (dwl): Super+P text picker, Super+Shift+P image picker.

## Status

Phase 1 (this commit): the kaprica CLI contract test harness in `tests/` — 6 C suites
(daemon, text, search, reverse, image, picker) that lock the CLI contract against an
isolated kapd. See `tests/docs/contract.md` and `tests/docs/run.md`.

Phase 2 (next): the `wclipmenu` picker binary wiring `kapc` into `wmenu`.

## Tests

```
cd tests && make clean && make && make test
```

Requires a live Wayland session, kaprica installed (/usr/local/bin/kapd + kapc), and
no system kapd running (kapd is a singleton). All suites use an isolated db under
/tmp — the real history db is never touched.

## Credits / inspiration

- [wmenu](https://sr.ht/~adnano/wmenu/) — the picker (dmenu-style, Wayland-native)
- [kaprica](https://github.com/kovidgoyal/kaprica) — clipboard daemon + CLI backend
- [cclip](https://github.com/erebe/cclip) — Rust Wayland clipboard manager (concepts)
- [dwl](https://codeberg.org/dwl/dwl) — the window manager / keybind host
- [chafa](https://hpjansson.org/chafa/) — image -> sixel conversion for previews
- [wl-clipboard](https://github.com/bugaevc/wl-clipboard) — Wayland clipboard utilities
- [foot](https://codeberg.org/dnkl/foot) — terminal with sixel support (preview target)
- [clipmenu](https://github.com/cdown/clipmenu) — the dmenu-based original this is modeled on

Special thanks to all of the above — wclipmenu is a thin glue layer standing on their work.

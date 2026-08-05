# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/), and
this project adheres to [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-08-04

First release.

### Added

- Text picker: pick from kaprica `text/plain` history via a wmenu menu,
  copy the selection back with `kapc copy`
- Image picker: `image/png` history with PNG thumbnails rendered through the
  wmenu-caos `[img:]` protocol (160x160 thumbs, cached, parallel forks)
- Subcommands: `list`, `image`, `copy`, `-h`
- Env overrides: `WCLIPMENU_WMENU`, `WCLIPMENU_LIMIT`, `WCLIPMENU_DB`
- Test harness: 6 C suites locking the kaprica CLI contract
- Man page (`wclipmenu.1`), sibling-style README, GitHub Actions CI

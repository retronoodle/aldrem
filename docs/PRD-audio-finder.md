# PRD: Audio file finder (gh issue #14)

## Goal

Let the user find audio files on disk without browsing folder by folder. They search a folder (or the whole machine), then add all results or a ticked subset to the current playlist.

## Requirements

- **Entry point:** a `Find audio` button in the playlist toolbar opens a modal dialog. It is disabled while another dialog is open, like the others.
- **Root:** a text field, defaulting to `$HOME` on first use and remembered for the session. Typing `/` searches the whole machine. Enter or `Search` starts the scan.
- **Matching:** the same supported-format list as the rest of the app (`track_ext_supported`). Hidden files and folders are skipped. Symlinked folders are not followed, so loops cannot happen. Symlinked files count, as `playlist_add` follows them.
- **Results:** sorted by path, one row per file with a checkbox. Rows can be ticked only after the scan finishes, so the sorted order cannot shift under the boxes. `Select all`, `Select none`, `Add selected` and `Add all` act on the result list.
- **Adding:** one batch add, with one autosave for the whole batch. Duplicates already in the playlist are skipped silently. If the playlist is idle, the first new track starts, as with a drop.
- **Responsiveness:** the scan runs in slices from the frame loop (a fixed number of directory entries per frame), so the window stays live. `Stop` or closing the dialog cancels it and keeps what was found.
- **Limits:** at most 10000 results, matching the playlist cap. The status line says when the list is capped.
- **Constraints:** C99, no new dependencies, no threads. The scanner (`src/finder.c`) has no SDL or Nuklear dependency.

## Why this is not a library

The app is a player, not a music library. Nothing is indexed, nothing is stored between sessions, and no tags are read. A search gives a transient list that is either added to the playlist or thrown away. Anything persistent, such as a folder watch, an index or metadata, is out of scope and would need its own PRD.

## Out of scope

- Recursive watching, indexing, or tag-based search.
- Expanding `~` in the root field.
- Native OS file dialogs.

## Tasks

1. Scanner: `src/finder.c` / `src/finder.h`, with `tests/test_finder.c`.
2. Playlist: `playlist_add_many` in `src/playlist.c`, sharing the per-file checks with `playlist_add`.
3. UI: `Find audio` button and modal in `src/main.c`.
4. Build: sources, headers and the test target in `Makefile`.
5. Docs: this PRD, plus lines in `README.md` and `CONTRIBUTING.md`.

# PRD: Internet radio (gh issue #6)

## Goal

Let the user find and play internet radio stations inside Aldrem. They search the open Radio Browser directory by name, then play a station or add stations to the playlist. A station is an ordinary playlist entry, so the queue, prev/next, auto-advance and autosave all work as they do for files.

## Requirements

- **Entry point:** a `Radio` button in the playlist toolbar, beside `Find audio`. It is shown only in builds with libcurl (`net_available()`), disabled while another dialog is open, and hidden under `NET=0`.
- **Search:** a `Radio` dialog with a name field. Enter or `Search` starts a query against Radio Browser. The search runs on a worker thread, so the window stays responsive. `Stop` (or closing the dialog) cancels it and drops its results.
- **Mirrors:** the mirror list comes from `https://all.api.radio-browser.info/json/servers`. If it cannot be fetched, `de1.api.radio-browser.info` is used. Mirror hosts are checked against `[A-Za-z0-9.-]` before use.
- **Matching:** `/json/stations/search` with `hidebroken=true`, ordered by click count, capped at 200 results. Only stations with an `http(s)` `url_resolved` are kept. Only codecs the build can decode are kept: MP3 and Ogg always, AAC/AAC+ only when built with FFmpeg (`AAC=1`).
- **Results:** one row per station, shown as `name - country - codec bitrate`. Clicking a row selects it. Checkboxes pick stations to add. `Play` plays the selected row. `Add selected` and `Add all` add to the playlist.
- **Adding:** one batch add, with one autosave for the batch. A station already in the playlist is skipped silently. As with files, the first new entry starts when the player is idle.
- **Playing:** a stream opens with a connect timeout of 5 s and a first-data timeout of 8 s. The time label shows elapsed time and `LIVE`. The seek bar is disabled. Pause on a live stream stops it, and Play reconnects, so no stale audio is played. Prev on row 0 reconnects. Titles and row labels use the station name.
- **Stream playback:** the codec comes from `Content-Type` (`audio/mpeg`, `audio/ogg`, `audio/aac`, `audio/aacp`, `audio/x-aac`), and falls back to sniffing the first bytes (`OggS`, `ID3`, MPEG sync, ADTS).
- **Reconnects:** when a connection drops, the decoder reconnects up to 3 times with backoff before the stream ends. Audio already received is played before the reconnect, so a short drop costs at most the audio in flight. Auto-advance runs only after the final failure.
- **Playlist format:** a station is written to `.m3u` as `#EXTINF:-1,Name` followed by its URL. URL lines are read verbatim, with no `~` expansion or path joining. Files without a name are written as before.
- **Limits:** 10000 playlist entries in total, as before. Stations are capped at 200 per search.
- **Constraints:** C99. New dependencies are libcurl (required for the feature) and FFmpeg's libavcodec and libavutil (optional, AAC only). Both are linked through pkg-config and auto-detected. `make NET=0 AAC=0` builds the SDL2-only binary, with no network code reachable from the UI.

## Why this is not a general streaming service

The feature is a directory client plus a live decoder, nothing more. No account, no catalogue, no history, no recommendations. Stations come from Radio Browser, a community directory with no API key. Nothing is stored between sessions except the playlist that already autosaves.

## Licensing

- **AAC:** libavcodec is LGPL. It is linked dynamically and only the AAC decoder is used. No permissively licensed C AAC decoder exists: faad2 is GPL and fdk-aac is not free software, and Aldrem is MIT. Builds without FFmpeg still work, and AAC stations are then hidden.
- **libcurl:** MIT/curl license, linked through the system library.
- **jsmn:** MIT, vendored unmodified in `vendor/jsmn.h` (see `vendor/VENDORED.md`).

## Security notes

The network surface is deliberately small:

- Schemes are limited to `http` and `https`, including redirects. Redirects are capped at 5.
- Cookies are off, and no credentials are sent. The User-Agent is `Aldrem/<version>`.
- Response bodies are size-capped: 64 KiB for the mirror list, 2 MiB for a search. Stream buffers are fixed-size.
- Stream URLs from playlists are opened only when played. They are not checked before that.
- JSON is parsed with a bounded token parser. Strings are truncated to their buffers.

## Out of scope

- ICY/stream metadata (now-playing text). Requests send `Icy-MetaData: 0`.
- `.pls` and `.m3u` station URLs (Radio Browser already returns a direct stream URL).
- HLS (`.m3u8`) streams, Ogg Opus, and multichannel streams.
- Recording, favourites, sorting or filtering the results.
- Async stream open. Opening waits on the network, so a dead station can freeze the UI for up to about 8 s. Async open is a follow-up.

## Known limits

- **Open is synchronous.** `Play` and playlist starts block the UI thread while the stream connects (up to about 8 s).
- **Stop latency.** A stop wakes a blocked read at once. A reconnect that is already inside `net_open` cannot be woken, so stop waits for that attempt (up to about 8 s).
- **Shutdown.** Closing the app while a search is in flight waits for its current fetch (up to about 10 s).
- **Station hint not used.** The Radio Browser codec is not passed to the decoder. Content-Type and sniffing decide the codec.
- **Rate changes.** A mid-stream sample-rate or channel change ends the stream with an error. HE-AAC streams are read at the rate of their first decoded frames.

## Tasks

1. Radio Browser client: `src/radio.c` / `src/radio.h` (pure URL building and JSON parsing), with `tests/test_radio.c`. JSON is parsed with the vendored `vendor/jsmn.h`.
2. Networking: `src/net.c` / `src/net.h` (libcurl wrapper, a byte ring fed by a transfer thread, `net_fetch` for small bodies), with `tests/test_net.c` against a loopback HTTP server.
3. Live decoding: `src/track_stream.c` (MP3, Ogg Vorbis, reconnects, `track_open` routing for `http(s)://`) and `src/track_stream_aac.c` (libavcodec, behind `HAVE_AVCODEC`), with `tests/test_stream.c` and small committed fixtures in `tests/data/`.
4. Playback: `src/track.h` / `src/track.c` (`TrackInfo.is_live`, `track_interrupt`), `src/playback.c` / `src/playback.h` (live status, seek refused, pause means stop, interrupt before joining the decoder).
5. Playlist: `src/playlist.c` / `src/playlist.h` (station entries, `#EXTINF` names kept in step with paths, `playlist_add_stations`, `playlist_label_at`), with `tests/test_playlist.c`.
6. Search worker: `src/radio_search.c` / `src/radio_search.h`, which runs the fetch and parse on its own thread.
7. UI: `src/main.c` (`Radio` button, `ui_radio_modal`, live time label, disabled seek bar, station labels).
8. Build and CI: `Makefile` (`NET`, `AAC`, the new sources and test targets), `.github/workflows/build.yml` (libcurl and FFmpeg on Linux, curl and ffmpeg via Homebrew on macOS, and one `NET=0 AAC=0` leg).
9. Docs: this PRD, plus `README.md`, `CONTRIBUTING.md` and `SECURITY.md`.

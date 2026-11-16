# Playback and shutdown regression checks

Run `ctest --test-dir build -C Release --output-on-failure` (also with `Debug`).

- `performance_unit` interrupts an hour-long presentation deadline and verifies
  that a private message-only window is destroyed during exception unwinding.
- `import_and_decoder` decodes a deliberately damaged FFV1 fixture. Opening the
  container succeeds, but corrupted packets fail on the decoder thread. The
  consumer must exit and `Decoder::rethrow_error()` must expose the exception
  after `join()`. Playback forwards this exception through `errorOccurred`.
- `atomic_import_worker` destroys `WallpaperAPI` immediately after starting
  imports, and after allowing imports to run, without processing GUI events.
  Shutdown must cancel/join workers and leave no temporary import files.

The GUI uses one state refresh routine for selection, playback status, import
completion, stopping, and automatic startup. Import completion resets the
progress dialog rather than closing it as if the user had cancelled.

Tests use private windows and isolated library paths, not the desktop wallpaper
or the user's media collection. These are correctness checks, not performance
benchmarks or a guarantee for every codec/device combination.

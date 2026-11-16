# Performance changes and validation

## Playback

- Hardware-to-CPU frame downloads no longer lose presentation timing: PTS is
  taken from the original decoder frame, including its best-effort fallback.
- Software pixel conversion writes directly into a reference-counted
  `AVBufferPool` allocation. Queuing the result retains a reference rather than
  allocating and copying another entire YUV image. Buffers return to the pool
  only after all consumers release them.
- Both SDL and D3D11 paths discard frames more than the larger of 100 ms and
  the preceding frame interval behind their PTS deadline, before texture
  upload, copy, draw or presentation. Low-FPS frames therefore remain useful
  for their full interval. This reduces
  wasted rendering work during stalls; it does not eliminate decoding work.
- Looping drains delayed decoder frames before seeking, including B-frames.
  The monotonic presentation clock resets on short loops and on resume.
- D3D11's shared context lock covers copy/draw submission, not `Present`.
  Application-created shared devices enable video support and
  `ID3D10Multithread` protection, as FFmpeg does for devices it creates itself.
  This is required to safely use the shared immediate context across decoding
  and presentation threads.
- A hidden wallpaper still stops consuming the bounded queue, naturally
  blocking the decoder rather than decoding discarded frames indefinitely.

The existing GPU path is CPU-zero-copy, not GPU-copy-free: it still uses
`CopySubresourceRegion` into a shader-readable texture. Do not remove this copy
without checking D3D11 decoder-surface restrictions and driver compatibility.

## Import

- Compatible YUV420P/YUVJ420P H.264 videos no larger than the screen use a
  cancellable byte-copy path and retain their original container and quality.
- Other videos are scaled down only when necessary, retain their aspect ratio,
  and use even dimensions for YUV420. Small videos are never upscaled.
- Encoding tries `h264_nvenc`, `h264_qsv`, `h264_amf`, then `libx264`. A hardware
  encoder is selected only if it can actually be opened on this machine.
  Hardware encoding uses NV12 input and a bounded pixel/FPS-dependent bitrate;
  software fallback uses YUV420P, preset `fast`, CRF 23. These are not identical
  quality settings. Pass `prefer_hardware=false` to `transcode_video_to_screen`
  when deterministic software encoding is preferred.
- Import drains both decoder and encoder, supplies packet/frame durations,
  and checks writes, so delayed/tail frames are not silently truncated.
- The Qt worker writes a temporary file beside the destination and renames it
  only on success. Existing destination files are never overwritten/deleted on
  failure. Cancellation removes the temporary file.

## Reproducing checks

```powershell
./scripts/build.ps1 -QtRoot C:/Qt/6.11.1/msvc2022_64
ctest --test-dir build -C Release --output-on-failure
./scripts/build.ps1 -QtRoot C:/Qt/6.11.1/msvc2022_64 -Configuration Debug
ctest --test-dir build -C Debug --output-on-failure
```

Coverage includes deterministic clock/drop behavior, original PTS selection,
buffer reuse/lifetime/resizing, bounded aspect-preserving dimensions, queue
cancellation, byte-identical fast imports, full B-frame preservation,
hardware/software encoder selection, actual decoding across loop boundaries,
Unicode paths, atomic publication and shared-device D3D11 rendering.

The GPU smoke test renders into its own hidden window; it does not validate
desktop visibility behavior or final on-screen color output. CTest timeouts
detect hangs. No CPU/GPU utilization or FPS improvement is claimed from these
correctness tests alone: profile the same video in Release before/after using
WPR/WPA or GPUView, separately for visible, covered and software-fallback modes.

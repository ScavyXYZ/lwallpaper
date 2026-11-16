#include "wallpaper/wallpaper_core.hpp"
#include "render/d3d11_zero_copy.hpp"
#include "wallpaper/window_handle.hpp"

#include <array>
#include <future>

namespace {
    void check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    void expect_close(double actual, double expected, const char* message) {
        check(std::abs(actual - expected) < 0.000001, message);
    }

    void unit_tests() {
        check(SDL_GetVersion() == SDL_VERSION, "Loaded SDL DLL does not match the installed headers");
        check(AV_VERSION_MAJOR(avcodec_version()) == LIBAVCODEC_VERSION_MAJOR,
            "Loaded FFmpeg DLL does not match the installed headers");
        std::cout << "Runtime: FFmpeg " << av_version_info() << ", SDL " << SDL_GetVersion() << '\n';
        wallpaper::PlaybackClock clock;
        clock.on_frame(1.0, 10.0);
        expect_close(clock.time_until(1.04, 10.02), 0.02, "Incorrect presentation deadline");
        check(!clock.should_drop(1.04, 10.08), "Slightly late frame should be retained");
        check(clock.should_drop(1.04, 10.20), "Stale frame should be dropped");
        clock.on_frame(1.04, 10.20);
        check(clock.should_drop(1.04, 10.20), "Normal frames must not reset lateness");
        clock.resync();
        clock.on_frame(1.08, 100.0);
        expect_close(clock.time_until(1.08, 100.0), 0.0, "Resume should resync the clock");
        clock.on_frame(0.20, 100.2);
        clock.on_frame(0.0, 100.3);
        expect_close(clock.time_until(0.0, 100.3), 0.0, "Short video loops must resync");

        wallpaper::PlaybackClock slow_clock;
        wallpaper::PlaybackClock cancellable_clock;
        cancellable_clock.on_frame(0.0);
        std::atomic<bool> stop_wait{false};
        auto waiting = std::async(std::launch::async, [&]() {
            return cancellable_clock.wait_for(3600.0, stop_wait);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop_wait = true;
        check(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
            "Stopping must interrupt a long PTS wait");
        check(!waiting.get(), "Cancelled clock wait must not present the frame");

        HWND test_window = CreateWindowExW(0, L"STATIC", L"Cleanup test", 0,
            0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(test_window != nullptr, "Cannot create isolated test window");
        try {
            wallpaper::WindowHandle handle(test_window);
            throw std::runtime_error("Simulated decoder initialization failure");
        } catch (const std::runtime_error&) {}
        check(!IsWindow(test_window), "Exception unwinding must destroy the child window");

        slow_clock.on_frame(0.0, 10.0);
        slow_clock.on_frame(0.5, 10.5);
        check(!slow_clock.should_drop(0.5, 10.9), "Low-FPS frames should remain visible for their interval");
        check(slow_clock.should_drop(0.5, 11.1), "Low-FPS frames must still drop when obsolete");

        wallpaper::VideoFramePool pool;
        auto original = pool.acquire(160, 90);
        original->pts = 42;
        original->best_effort_timestamp = 43;
        expect_close(wallpaper::frame_pts_seconds(*original, 0.04), 1.68, "Original PTS must win");
        original->pts = AV_NOPTS_VALUE;
        expect_close(wallpaper::frame_pts_seconds(*original, 0.04), 1.72, "Best-effort timestamp fallback");
        original->best_effort_timestamp = AV_NOPTS_VALUE;
        expect_close(wallpaper::frame_pts_seconds(*original, 0.04), 0.0, "Missing timestamp fallback");

        uint8_t* first = original->data[0];
        std::memset(first, 0x55, original->buf[0]->size);
        ffmpeg::FramePtr queued(av_frame_clone(original.get()));
        check(queued != nullptr, "Cannot clone pooled frame");
        original.reset();
        auto second = pool.acquire(160, 90);
        check(second->data[0] != first, "Queued data must not be reused while referenced");
        queued.reset();
        auto recycled = pool.acquire(160, 90);
        check(recycled->data[0] == first, "Released buffer should be reused");
        auto resized = pool.acquire(320, 180);
        check(resized->width == 320, "Pool must support dimension changes");
        check(recycled->data[0][0] == 0x55, "Resetting pool must not invalidate queued frames");

        auto size = wallpaper::import_size({1280, 720}, {3840, 2160});
        check(size.w == 1280 && size.h == 720, "Imports must not upscale");
        size = wallpaper::import_size({3840, 2160}, {1920, 1200});
        check(size.w == 1920 && size.h == 1080, "Imports must preserve aspect ratio");
        size = wallpaper::import_size({1080, 1920}, {1920, 1080});
        check(size.w == 606 && size.h == 1080, "Portrait output must fit and have even dimensions");
        bool invalid_rejected = false;
        try { wallpaper::import_size({0, 90}, {1920, 1080}); }
        catch (const std::exception&) { invalid_rejected = true; }
        check(invalid_rejected, "Invalid dimensions must be rejected");

        FrameQueue queue(1);
        std::atomic<bool> cancelled{false}, done{false};
        check(queue.push(YUVFrame{}, cancelled), "Initial queue push failed");
        auto blocked = std::async(std::launch::async, [&]() { return queue.push(YUVFrame{}, cancelled); });
        cancelled = true;
        queue.cancel();
        check(blocked.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "Cancellation must wake a blocked producer");
        check(!blocked.get(), "Cancelled push should fail");
        std::cout << "PASS: clock, late-frame dropping, timestamps, pooled buffers, import sizing, cancellation\n";
    }

    struct VideoInfo { int width, height, frames; AVCodecID codec; };

    VideoInfo inspect_video(const std::string& path) {
        AVFormatContext* raw = nullptr;
        check(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0, "Cannot open output video");
        ffmpeg::FormatContextPtr format(raw);
        check(avformat_find_stream_info(format.get(), nullptr) >= 0, "Cannot probe output video");
        const AVCodec* codec = nullptr;
        int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        check(stream >= 0, "Output has no video stream");
        ffmpeg::CodecContextPtr decoder(avcodec_alloc_context3(codec));
        check(decoder != nullptr, "Cannot allocate verification decoder");
        check(avcodec_parameters_to_context(decoder.get(), format->streams[stream]->codecpar) >= 0,
            "Cannot configure verification decoder");
        check(avcodec_open2(decoder.get(), codec, nullptr) >= 0, "Cannot open verification decoder");
        ffmpeg::FramePtr frame(av_frame_alloc());
        ffmpeg::PacketPtr packet(av_packet_alloc());
        VideoInfo result{decoder->width, decoder->height, 0, codec->id};
        auto receive = [&]() {
            int status;
            while ((status = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
                ++result.frames;
                av_frame_unref(frame.get());
            }
            check(status == AVERROR_EOF || status == AVERROR(EAGAIN), "Output decode error");
        };
        while (av_read_frame(format.get(), packet.get()) >= 0) {
            if (packet->stream_index == stream) {
                check(avcodec_send_packet(decoder.get(), packet.get()) >= 0, "Output packet decode error");
                receive();
            }
            av_packet_unref(packet.get());
        }
        check(avcodec_send_packet(decoder.get(), nullptr) >= 0, "Output decoder drain error");
        receive();
        return result;
    }

    bool equal_files(const std::filesystem::path& a, const std::filesystem::path& b) {
        if (std::filesystem::file_size(a) != std::filesystem::file_size(b)) return false;
        std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
        return std::equal(std::istreambuf_iterator<char>(left), std::istreambuf_iterator<char>(),
            std::istreambuf_iterator<char>(right));
    }

    void decoder_test(const std::string& path, bool force_conversion) {
        FrameQueue queue(kFrameQueueCapacity);
        std::atomic<bool> cancelled{false};
        Decoder decoder(path, queue, cancelled);
        if (force_conversion) decoder.nv12_passthrough_ok = false;
        decoder.start();
        double previous = -1.0;
        for (int i = 0; i < 30; ++i) {
            YUVFrame frame;
            check(queue.pop(frame, cancelled, decoder.done_flag()), "Decoder stopped unexpectedly");
            check(frame.frame != nullptr && !frame.is_raw_hw_frame, "Expected a CPU/SDL-compatible frame");
            if (i % 24 == 0) previous = -1.0;
            check(frame.pts_seconds > previous, "Decoded timestamps were lost or reordered");
            check(std::abs(frame.pts_seconds - (i % 24) / 24.0) < 0.001,
                "Decoder lost timestamps or frames across a loop");
            previous = frame.pts_seconds;
            if (force_conversion) check(!frame.is_nv12 && frame.frame->format == AV_PIX_FMT_YUV420P,
                "Forced conversion must produce YUV420P");
        }
        cancelled = true;
        queue.cancel();
        decoder.join();
        decoder.rethrow_error();
    }

    void media_tests(const std::string& h264, const std::string& ffv1, const std::string& scratch) {
        std::atomic<bool> cancelled{false};
        auto output = [&](const char* name) { return (std::filesystem::u8path(scratch) / name).u8string(); };
        const auto copy = output("copied.mp4");
        auto result = wallpaper::transcode_video_to_screen(h264, copy, cancelled, {1920, 1080}, false);
        check(result.copied, "Copy import should report its fast path");
        check(equal_files(std::filesystem::u8path(h264), std::filesystem::u8path(copy)),
            "Compatible H264 must be copied byte-for-byte");

        const auto downscaled = output("small.mp4");
        wallpaper::transcode_video_to_screen(h264, downscaled, cancelled, {96, 64}, false);
        auto info = inspect_video(downscaled);
        check(info.codec == AV_CODEC_ID_H264 && info.width == 96 && info.height == 54,
            "Transcoding must preserve aspect ratio and respect the limit");
        check(info.frames == 24, "B-frames must not be lost when draining the decoder/encoder");

        const auto converted = output("converted.mp4");
        wallpaper::transcode_video_to_screen(ffv1, converted, cancelled, {1920, 1080}, false);
        info = inspect_video(converted);
        check(info.width == 160 && info.height == 90 && info.frames == 24,
            "Unsupported formats must be converted without upscaling or truncation");

        const auto hardware = output("auto-encoder.mp4");
        result = wallpaper::transcode_video_to_screen(ffv1, hardware, cancelled, {1920, 1080}, true);
        info = inspect_video(hardware);
        check(info.width == 160 && info.height == 90 && info.frames == 24,
            "Automatic hardware/software encoder selection failed");
        std::cout << "Automatic import encoder: " << result.encoder << '\n';

        const auto stopped = output("cancelled.mp4");
        cancelled = true;
        bool rejected = false;
        try { wallpaper::transcode_video_to_screen(h264, stopped, cancelled, {1920, 1080}, false); }
        catch (const std::exception&) { rejected = true; }
        check(rejected && !std::filesystem::exists(std::filesystem::u8path(stopped)),
            "Cancelled fast import must not publish output");

        decoder_test(h264, false);
        decoder_test(h264, true);
        decoder_test(ffv1, true);
        {
            FrameQueue queue(kFrameQueueCapacity);
            std::atomic<bool> stop{false};
            Decoder damaged(output("damaged.mkv"), queue, stop);
            damaged.start(); // Opening succeeds; corrupted packets fail on the worker thread.
            auto consume = std::async(std::launch::async, [&]() {
                YUVFrame frame;
                while (queue.pop(frame, stop, damaged.done_flag())) {}
            });
            const bool exited = consume.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
            stop = true;
            queue.cancel();
            consume.get();
            damaged.join();
            check(exited, "Decoder error must wake and stop the consumer");
            bool reported = false;
            try { damaged.rethrow_error(); }
            catch (const std::exception& error) {
                reported = std::string(error.what()).find("Failed to") != std::string::npos;
            }
            check(reported, "Background decoder errors must reach the playback caller");
        }
        std::cout << "PASS: byte-copy import, downscale, no upscale, encoder fallback, B-frame drain, decoder PTS/conversion\n";
    }

    int gpu_test(const std::string& path) {
        // A private hidden window; never attach to WorkerW or modify the desktop.
        HWND window = CreateWindowExW(0, L"STATIC", L"LWallpaper GPU test", WS_OVERLAPPEDWINDOW,
            0, 0, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(window != nullptr, "Cannot create GPU test window");
        struct WindowGuard { HWND hwnd; ~WindowGuard() { DestroyWindow(hwnd); } } window_guard{window};
        wallpaper::D3D11ZeroCopyRenderer renderer;
        if (!renderer.initialize(window, 160, 90)) {
            std::cout << "SKIP: GPU renderer unavailable: " << renderer.last_error() << '\n';
            return 77;
        }
        FrameQueue queue(kFrameQueueCapacity);
        std::atomic<bool> cancelled{false};
        Decoder decoder(path, queue, cancelled);
        decoder.external_d3d_device = renderer.device();
        decoder.start();
        renderer.set_sync_callbacks(decoder.hw_lock, decoder.hw_unlock, decoder.hw_lock_ctx);
        for (int i = 0; i < 24; ++i) {
            YUVFrame frame;
            check(queue.pop(frame, cancelled, decoder.done_flag()), "GPU decoder stopped");
            if (!frame.is_raw_hw_frame) {
                std::cout << "SKIP: hardware decode unavailable\n";
                return 77;
            }
            if (!renderer.present_frame(frame.frame.get())) throw std::runtime_error(renderer.last_error());
        }
        cancelled = true;
        queue.cancel();
        decoder.join();
        std::cout << "PASS: shared-device D3D11 decode/copy/draw/present (24 frames)\n";
        return 0;
    }
}

int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);
    try {
        if (argc == 2 && std::string(argv[1]) == "--unit") unit_tests();
        else if (argc == 5 && std::string(argv[1]) == "--media") media_tests(argv[2], argv[3], argv[4]);
        else if (argc == 3 && std::string(argv[1]) == "--gpu") return gpu_test(argv[2]);
        else throw std::runtime_error("Invalid test arguments");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

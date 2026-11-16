#pragma once

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include <memory>
#include <stdexcept>

namespace wallpaper {
    // References held by queued frames remain valid even after the pool is reset.
    class VideoFramePool {
    public:
        struct FrameDeleter {
            void operator()(AVFrame* frame) const { av_frame_free(&frame); }
        };
        using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

        ~VideoFramePool() { av_buffer_pool_uninit(&pool_); }
        VideoFramePool() = default;
        VideoFramePool(const VideoFramePool&) = delete;
        VideoFramePool& operator=(const VideoFramePool&) = delete;

        FramePtr acquire(int width, int height, AVPixelFormat format = AV_PIX_FMT_YUV420P) {
            if (!pool_ || width != width_ || height != height_ || format != format_) {
                int size = av_image_get_buffer_size(format, width, height, 32);
                if (size < 0) throw std::runtime_error("Invalid video frame dimensions/format");
                AVBufferPool* replacement = av_buffer_pool_init(size, nullptr);
                if (!replacement) throw std::runtime_error("Cannot allocate video frame pool");
                av_buffer_pool_uninit(&pool_);
                pool_ = replacement;
                width_ = width;
                height_ = height;
                format_ = format;
            }

            FramePtr frame(av_frame_alloc());
            if (!frame) throw std::runtime_error("Cannot allocate video frame");
            frame->buf[0] = av_buffer_pool_get(pool_);
            if (!frame->buf[0]) throw std::runtime_error("Cannot acquire video frame buffer");
            frame->format = format;
            frame->width = width;
            frame->height = height;
            if (av_image_fill_arrays(frame->data, frame->linesize, frame->buf[0]->data,
                format, width, height, 32) < 0) {
                throw std::runtime_error("Cannot initialize video frame planes");
            }
            return frame;
        }

    private:
        AVBufferPool* pool_ = nullptr;
        int width_ = 0, height_ = 0;
        AVPixelFormat format_ = AV_PIX_FMT_NONE;
    };

    inline double frame_pts_seconds(const AVFrame& original, double time_base) {
        int64_t pts = original.pts;
        if (pts == AV_NOPTS_VALUE) pts = original.best_effort_timestamp;
        return pts == AV_NOPTS_VALUE ? 0.0 : pts * time_base;
    }
}

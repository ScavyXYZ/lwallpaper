#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace wallpaper {
    class PlaybackClock {
    public:
        static double now() {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void on_frame(double pts, double wall_time = now()) {
            // Even clips shorter than half a second must reset when looping.
            if (!need_sync_ && pts < last_pts_ - 0.001) need_sync_ = true;
            if (!need_sync_ && pts > last_pts_) frame_interval_ = pts - last_pts_;
            last_pts_ = pts;
            if (need_sync_) {
                frame_interval_ = 0.0;
                pts_origin_ = pts;
                wall_origin_ = wall_time;
                need_sync_ = false;
            }
        }

        void resync() { need_sync_ = true; }

        double time_until(double pts, double wall_time = now()) const {
            return (pts - pts_origin_) - (wall_time - wall_origin_);
        }

        bool should_drop(double pts, double wall_time = now()) const {
            // Do not catch up by uploading/presenting a burst of stale frames.
            // A low-FPS frame remains useful for longer than 100 ms.
            return time_until(pts, wall_time) < -(std::max)(0.100, frame_interval_);
        }

        bool wait_for(double pts, const std::atomic<bool>& cancelled) const {
            double remaining;
            while (!cancelled.load() && (remaining = time_until(pts)) > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>((std::min)(remaining, 0.010)));
            }
            return !cancelled.load();
        }

    private:
        double pts_origin_ = 0.0;
        double wall_origin_ = 0.0;
        double last_pts_ = 0.0;
        double frame_interval_ = 0.0;
        bool need_sync_ = true;
    };
}

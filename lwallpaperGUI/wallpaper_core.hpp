#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

#include <SDL3/SDL.h>
#include <windows.h>
#include <shellscalingapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define DEBUG_ENABLED 0
#if DEBUG_ENABLED
#include <rang.hpp>
#define LOG_INFO_ENABLED 1
#define LOG_ERROR_ENABLED 1
#define LOG_WARN_ENABLED 1
#endif

#if DEBUG_ENABLED
#define DEBUG_LOG(stream, msg, color) \
        do { \
            static std::mutex log_mutex; \
            std::ostringstream oss; \
            auto now = std::chrono::system_clock::now(); \
            auto now_time_t = std::chrono::system_clock::to_time_t(now); \
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000; \
            struct tm now_tm; \
            localtime_s(&now_tm, &now_time_t); \
            oss << "[DEBUG " << std::put_time(&now_tm, "%H:%M:%S") \
                << '.' << std::setfill('0') << std::setw(3) << ms.count() \
                << "][" << std::this_thread::get_id() << "] " << msg << "\n"; \
            { \
                std::lock_guard<std::mutex> lock(log_mutex); \
                (stream) << color; \
                (stream) << oss.str(); \
                (stream).flush(); \
            } \
        } while(0)
#else
#define DEBUG_LOG(stream, msg, color) ((void)0)
#endif

#if LOG_INFO_ENABLED
#define LOG_INFO(msg) DEBUG_LOG(std::cout, "[INFO] " << msg, rang::fg::cyan)
#else
#define LOG_INFO(msg) ((void)0)
#endif

#if LOG_ERROR_ENABLED
#define LOG_ERROR(msg) DEBUG_LOG(std::cerr, "[ERROR] " << msg, rang::fg::red)
#else
#define LOG_ERROR(msg) \
    do { \
        static std::mutex log_mutex; \
        std::ostringstream oss; \
        oss << msg; \
        throw std::runtime_error(oss.str()); \
    } while(0)
#endif

#if LOG_WARN_ENABLED
#define LOG_WARN(msg) DEBUG_LOG(std::cerr, "[WARN] " << msg, rang::fg::yellow)
#else
#define LOG_WARN(msg) ((void)0)
#endif

namespace ffmpeg {
	struct FormatContextDeleter {
		void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
	};
	using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

	struct CodecContextDeleter {
		void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
	};
	using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
	struct FrameDeleter {
		void operator()(AVFrame* p) const { av_frame_free(&p); }
	};
	using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

	struct PacketDeleter {
		void operator()(AVPacket* p) const { av_packet_free(&p); }
	};
	using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

	struct SwsContextDeleter {
		void operator()(SwsContext* p) const { sws_freeContext(p); }
	};
	using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

	struct BufferRefDeleter {
		void operator()(AVBufferRef* p) const { av_buffer_unref(&p); }
	};
	using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

	struct AvBuffer {
		uint8_t* data = nullptr;
		explicit AvBuffer(int size) : data(static_cast<uint8_t*>(av_malloc(size))) {
			if (!data) throw std::runtime_error("av_malloc failed");
		}
		~AvBuffer() { av_free(data); }
		AvBuffer(const AvBuffer&) = delete;
		AvBuffer& operator=(const AvBuffer&) = delete;
	};

	inline std::string av_error(int errnum) {
		char buf[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(errnum, buf, sizeof(buf));
		return buf;
	}
}

struct YUVFrame {
	ffmpeg::FramePtr frame;
	double pts_seconds{};
	int width{};
	int height{};

	YUVFrame() : frame(nullptr) {}

	YUVFrame(YUVFrame&& other) noexcept = default;
	YUVFrame& operator=(YUVFrame&& other) noexcept = default;
};

class FrameQueue {
public:
	explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {
		LOG_INFO("FrameQueue created, capacity=" << capacity);
	}

	bool push(YUVFrame frame, const std::atomic<bool>& cancelled) {
		std::unique_lock lock(mutex_);
		cv_push_.wait(lock, [&] {
			return queue_.size() < capacity_ || cancelled.load();
			});
		if (cancelled) {
			LOG_INFO("push cancelled, queue size=" << queue_.size());
			return false;
		}
		queue_.push(std::move(frame));
		cv_pop_.notify_one();
		return true;
	}

	bool pop(YUVFrame& out, const std::atomic<bool>& cancelled,
		const std::atomic<bool>& producer_done) {
		std::unique_lock lock(mutex_);
		cv_pop_.wait(lock, [&] {
			return !queue_.empty() || producer_done.load() || cancelled.load();
			});
		if (cancelled || (queue_.empty() && producer_done)) {
			if (queue_.empty()) LOG_INFO("pop: queue empty and producer done");
			else LOG_INFO("pop cancelled");
			return false;
		}
		out = std::move(queue_.front());
		queue_.pop();
		cv_push_.notify_one();
		return true;
	}

	void cancel() {
		LOG_INFO("FrameQueue::cancel called");
		cv_push_.notify_all();
		cv_pop_.notify_all();
	}

	void clear() {
		std::lock_guard lock(mutex_);
		size_t sz = queue_.size();
		while (!queue_.empty()) queue_.pop();
		LOG_INFO("FrameQueue cleared, removed " << sz << " frames");
	}

private:
	std::size_t            capacity_;
	std::queue<YUVFrame>   queue_;
	std::mutex             mutex_;
	std::condition_variable cv_push_, cv_pop_;
};

class Decoder {
public:
	Decoder(const std::string& path, FrameQueue& queue, std::atomic<bool>& cancelled)
		: path_(path), queue_(queue), cancelled_(cancelled) {
		LOG_INFO("Decoder created for file: " << path);
	}

	void start() {
		open();
		thread_ = std::thread(&Decoder::run, this);
		LOG_INFO("Decoder thread started");
	}

	void join() {
		if (thread_.joinable()) {
			thread_.join();
			LOG_INFO("Decoder thread joined");
		}
	}

	void seek_start() {
		int64_t timestamp = 0;
		int ret = av_seek_frame(fmt_ctx_.get(), stream_idx_, timestamp, AVSEEK_FLAG_BACKWARD);
		if (ret < 0) {
			LOG_ERROR("Failed to seek to start: " << ffmpeg::av_error(ret));
			cancelled_ = true;
			return;
		}
		avcodec_flush_buffers(codec_ctx_.get());
		LOG_INFO("Seeked to beginning of file");
	}

	bool done() const { return done_.load(); }
	std::atomic<bool>& done_flag() { return done_; }
private:
	void open() {
		LOG_INFO("Opening input: " << path_);
		AVFormatContext* raw = nullptr;
		int err = avformat_open_input(&raw, path_.c_str(), nullptr, nullptr);
		if (err < 0)
			throw std::runtime_error("Cannot open video '" + path_ + "': " + ffmpeg::av_error(err));
		fmt_ctx_.reset(raw);
		LOG_INFO("avformat_open_input succeeded");

		err = avformat_find_stream_info(fmt_ctx_.get(), nullptr);
		if (err < 0)
			throw std::runtime_error("Cannot read stream info: " + ffmpeg::av_error(err));
		LOG_INFO("Stream info found, duration=" << fmt_ctx_->duration / AV_TIME_BASE << " sec");

		const AVCodec* codec = nullptr;
		stream_idx_ = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
		if (stream_idx_ < 0)
			throw std::runtime_error("No video stream found.");
		LOG_INFO("Video stream index=" << stream_idx_ << ", codec=" << codec->name);

		AVCodecContext* raw_cc = avcodec_alloc_context3(codec);
		if (!raw_cc)
			throw std::runtime_error("Cannot allocate codec context.");
		codec_ctx_.reset(raw_cc);

		avcodec_parameters_to_context(codec_ctx_.get(),
			fmt_ctx_->streams[stream_idx_]->codecpar);
		codec_ctx_->thread_count = 0;
		codec_ctx_->thread_type = FF_THREAD_FRAME;

		try_enable_hw_decode(codec);

		err = avcodec_open2(codec_ctx_.get(), codec, nullptr);
		if (err < 0)
			throw std::runtime_error("Cannot open codec: " + ffmpeg::av_error(err));
		LOG_INFO("Codec opened, resolution=" << codec_ctx_->width << "x" << codec_ctx_->height
			<< ", pix_fmt=" << av_get_pix_fmt_name(codec_ctx_->pix_fmt)
			<< ", hw_accel=" << (hw_accel_active_ ? "yes" : "no"));

		time_base_ = av_q2d(fmt_ctx_->streams[stream_idx_]->time_base);
		LOG_INFO("Time base=" << time_base_);

		if (hw_accel_active_) {
			LOG_INFO("Hardware decode active, deferring conversion setup until first frame");
			return;
		}

		needs_conversion_ = (codec_ctx_->pix_fmt != AV_PIX_FMT_YUV420P &&
			codec_ctx_->pix_fmt != AV_PIX_FMT_YUVJ420P);
		if (needs_conversion_) {
			LOG_INFO("Pixel format conversion required (to YUV420P)");
			build_sws();
		}
		else {
			LOG_INFO("Pixel format already YUV420P, no conversion");
		}
	}

	void try_enable_hw_decode(const AVCodec* codec) {
		for (int i = 0;; ++i) {
			const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
			if (!cfg) {
				LOG_INFO("No D3D11VA hw config available for codec " << codec->name << ", using software decode");
				return;
			}
			if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
				cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
				AVBufferRef* raw_hw_ctx = nullptr;
				int err = av_hwdevice_ctx_create(&raw_hw_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
				if (err < 0) {
					LOG_WARN("D3D11VA device creation failed: " << ffmpeg::av_error(err) << ", using software decode");
					return;
				}
				hw_device_ctx_.reset(raw_hw_ctx);
				codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_.get());
				hw_pix_fmt_ = cfg->pix_fmt;
				codec_ctx_->opaque = this;
				codec_ctx_->get_format = &Decoder::get_hw_format;
				hw_accel_active_ = true;
				LOG_INFO("D3D11VA hardware decode enabled for codec " << codec->name);
				return;
			}
		}
	}

	void build_sws() {
		int w = codec_ctx_->width, h = codec_ctx_->height;
		SwsContext* raw_sws = sws_getContext(w, h, codec_ctx_->pix_fmt,
			w, h, AV_PIX_FMT_YUV420P,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!raw_sws)
			throw std::runtime_error("Cannot create SwsContext.");
		sws_ctx_.reset(raw_sws);

		yuv_frame_.reset(av_frame_alloc());
		if (!yuv_frame_) throw std::runtime_error("av_frame_alloc failed for yuv_frame");
		int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codec_ctx_->width,
			codec_ctx_->height, 1);
		if (sz < 0) throw std::runtime_error("av_image_get_buffer_size failed");
		sws_buf_ = std::make_unique<ffmpeg::AvBuffer>(sz);
		av_image_fill_arrays(yuv_frame_->data, yuv_frame_->linesize,
			sws_buf_->data, AV_PIX_FMT_YUV420P,
			codec_ctx_->width, codec_ctx_->height, 1);
		LOG_INFO("SWS context created, conversion buffer size=" << sz);
	}

	void run() {
		LOG_INFO("Decoder thread main loop started");
		auto frame = ffmpeg::FramePtr(av_frame_alloc());
		if (!frame) {
			LOG_ERROR("av_frame_alloc failed in decoder thread");
			done_ = true;
			queue_.cancel();
			return;
		}
		auto packet = ffmpeg::PacketPtr(av_packet_alloc());
		if (!packet) {
			LOG_ERROR("av_packet_alloc failed");
			done_ = true;
			queue_.cancel();
			return;
		}

		int frame_count = 0;
		while (!cancelled_) {
			int ret = av_read_frame(fmt_ctx_.get(), packet.get());
			if (ret == AVERROR_EOF) {
				LOG_INFO("Reached EOF, looping...");
				seek_start();
				continue;
			}
			if (ret < 0) {
				LOG_WARN("av_read_frame error: " << ffmpeg::av_error(ret) << ", skipping");
				continue;
			}

			if (packet->stream_index == stream_idx_) {
				ret = avcodec_send_packet(codec_ctx_.get(), packet.get());
				if (ret == 0)
					drain_frames(frame.get(), frame_count);
				else if (ret < 0)
					LOG_WARN("avcodec_send_packet error: " << ffmpeg::av_error(ret));
			}
			av_packet_unref(packet.get());
		}

		done_ = true;
		LOG_INFO("Decoder thread finished, total frames decoded: " << frame_count);
		queue_.cancel();
	}

	void drain_frames(AVFrame* frame, int& frame_count) {
		while (!cancelled_) {
			int ret = avcodec_receive_frame(codec_ctx_.get(), frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
			if (ret < 0) {
				LOG_WARN("avcodec_receive_frame error: " << ffmpeg::av_error(ret));
				break;
			}

			ffmpeg::FramePtr sw_frame;
			AVFrame* decoded = frame;
			if (hw_accel_active_ && frame->format == hw_pix_fmt_) {
				sw_frame.reset(av_frame_alloc());
				if (av_hwframe_transfer_data(sw_frame.get(), frame, 0) < 0) {
					LOG_WARN("hwframe transfer failed, dropping frame");
					continue;
				}
				decoded = sw_frame.get();
				ensure_conversion_target(decoded);
			}

			AVFrame* src = decoded;
			if (needs_conversion_ && sws_ctx_) {
				sws_scale(sws_ctx_.get(), decoded->data, decoded->linesize,
					0, decoded->height,
					yuv_frame_->data, yuv_frame_->linesize);
				src = yuv_frame_.get();
			}

			YUVFrame yf = build_yuv_frame(src, decoded);
			if (!queue_.push(std::move(yf), cancelled_)) break;
			frame_count++;
			if (frame_count % 100 == 0) {
				LOG_INFO("Decoded " << frame_count << " frames");
			}
		}
	}

	void ensure_conversion_target(const AVFrame* transferred) {
		auto fmt = static_cast<AVPixelFormat>(transferred->format);
		int w = transferred->width;
		int h = transferred->height;

		if (sws_ctx_ && fmt == sws_src_fmt_ && w == sws_src_w_ && h == sws_src_h_) return;

		LOG_INFO("HW frame format: " << av_get_pix_fmt_name(fmt) << ", size=" << w << "x" << h);

		needs_conversion_ = (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P);
		sws_src_fmt_ = fmt;
		sws_src_w_ = w;
		sws_src_h_ = h;
		if (!needs_conversion_) { sws_ctx_.reset(); return; }

		SwsContext* raw_sws = sws_getContext(w, h, fmt,
			codec_ctx_->width, codec_ctx_->height, AV_PIX_FMT_YUV420P,
			SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!raw_sws) throw std::runtime_error("Cannot create SwsContext for hw-transferred frame.");
		sws_ctx_.reset(raw_sws);

		yuv_frame_.reset(av_frame_alloc());
		yuv_frame_->format = AV_PIX_FMT_YUV420P;
		yuv_frame_->width = codec_ctx_->width;
		yuv_frame_->height = codec_ctx_->height;
		int sz = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codec_ctx_->width, codec_ctx_->height, 1);
		sws_buf_ = std::make_unique<ffmpeg::AvBuffer>(sz);
		av_image_fill_arrays(yuv_frame_->data, yuv_frame_->linesize,
			sws_buf_->data, AV_PIX_FMT_YUV420P,
			codec_ctx_->width, codec_ctx_->height, 1);
	}

	YUVFrame build_yuv_frame(AVFrame* src, AVFrame* original) const {
		YUVFrame yf;
		yf.width = codec_ctx_->width;
		yf.height = codec_ctx_->height;

		int64_t pts_raw = original->pts;
		if (pts_raw == AV_NOPTS_VALUE) pts_raw = original->best_effort_timestamp;
		yf.pts_seconds = (pts_raw != AV_NOPTS_VALUE) ? pts_raw * time_base_ : 0.0;

		yf.frame.reset(av_frame_alloc());

		if (!needs_conversion_) {
			av_frame_ref(yf.frame.get(), src);
		}
		else {
			yf.frame->format = AV_PIX_FMT_YUV420P;
			yf.frame->width = codec_ctx_->width;
			yf.frame->height = codec_ctx_->height;
			av_frame_get_buffer(yf.frame.get(), 32);
			av_frame_copy(yf.frame.get(), src);
		}

		return yf;
	}

	std::string             path_;
	FrameQueue& queue_;
	std::atomic<bool>& cancelled_;
	std::atomic<bool>       done_{ false };
	std::thread             thread_;

	ffmpeg::FormatContextPtr fmt_ctx_;
	ffmpeg::CodecContextPtr  codec_ctx_;
	ffmpeg::SwsContextPtr    sws_ctx_;
	ffmpeg::FramePtr         yuv_frame_;
	std::unique_ptr<ffmpeg::AvBuffer> sws_buf_;
	ffmpeg::BufferRefPtr    hw_device_ctx_;

	int    stream_idx_ = -1;
	double time_base_ = 0.0;
	bool   needs_conversion_ = false;
	bool   hw_accel_active_ = false;
	AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
	int    sws_src_w_ = 0;
	int    sws_src_h_ = 0;
	AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;

	static AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
		auto* self = static_cast<Decoder*>(ctx->opaque);
		for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == self->hw_pix_fmt_) return *p;
		}
		LOG_WARN("Failed to get HW surface format, falling back to software");
		return pix_fmts[0];
	}
};

namespace wallpaper {
	struct ScreenSize { int w, h; };

	inline ScreenSize physical_screen_size() {
		DEVMODEA dm{};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
			LOG_INFO("Physical screen size via EnumDisplaySettings: " << dm.dmPelsWidth << "x" << dm.dmPelsHeight);
			return { static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight) };
		}
		int w = GetSystemMetrics(SM_CXSCREEN);
		int h = GetSystemMetrics(SM_CYSCREEN);
		LOG_INFO("Physical screen size via GetSystemMetrics: " << w << "x" << h);
		return { w, h };
	}

	inline HWND wallpaper_hwnd(int sw, int sh) {
		HWND progman = FindWindowA("Progman", nullptr);
		if (!progman) {
			LOG_ERROR("Progman window not found");
			return nullptr;
		}
		LOG_INFO("Progman HWND found: " << progman);
		SendMessageTimeoutA(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

		HWND worker = FindWindowExA(progman, nullptr, "WorkerW", nullptr);
		while (worker) {
			RECT r{};
			GetWindowRect(worker, &r);
			int w = r.right - r.left;
			int h = r.bottom - r.top;
			LOG_INFO("WorkerW candidate: HWND=" << worker << " size=" << w << "x" << h);
			if (std::abs(w - sw) <= 10 && std::abs(h - sh) <= 10) {
				LOG_INFO("Selected WorkerW as wallpaper window");
				return worker;
			}
			worker = FindWindowExA(progman, worker, "WorkerW", nullptr);
		}
		LOG_INFO("No suitable WorkerW found, using Progman as fallback");
		return progman;
	}

	inline LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		if (msg == WM_DESTROY) {
			LOG_INFO("Child window destroyed, posting quit");
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcA(hwnd, msg, wp, lp);
	}

	inline HWND create_wallpaper_child(HINSTANCE inst, HWND parent, int w, int h) {
		WNDCLASSEXA wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = wnd_proc;
		wc.hInstance = inst;
		wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.lpszClassName = "LiveWP";

		if (!RegisterClassExA(&wc)) {
			DWORD err = GetLastError();
			if (err != ERROR_CLASS_ALREADY_EXISTS) {
				LOG_ERROR("RegisterClassEx failed, error=" << err);
				throw std::runtime_error("RegisterClassExA failed");
			}
			LOG_INFO("Window class 'LiveWP' already registered, skipping registration.");
		}
		else {
			LOG_INFO("Window class registered successfully");
		}

		HWND hwnd = CreateWindowExA(0, "LiveWP", "wp",
			WS_CHILD | WS_VISIBLE,
			0, 0, w, h,
			parent, nullptr, inst, nullptr);
		if (!hwnd) {
			DWORD err = GetLastError();
			LOG_ERROR("CreateWindowEx failed, error=" << err);
			throw std::runtime_error("CreateWindowExA failed");
		}
		LOG_INFO("Child window created: HWND=" << hwnd << ", size=" << w << "x" << h);
		return hwnd;
	}

	inline void transcode_video_to_screen(const std::string& inputPath, const std::string& outputPath, std::atomic<bool>& cancelled) {
		AVFormatContext* inFmtCtx = nullptr;
		AVFormatContext* outFmtCtx = nullptr;
		AVCodecContext* decCtx = nullptr;
		AVCodecContext* encCtx = nullptr;
		SwsContext* swsCtx = nullptr;

		AVPacket* inPacket = nullptr;
		AVPacket* outPacket = nullptr;
		AVFrame* decFrame = nullptr;
		AVFrame* encFrame = nullptr;

		int videoStreamIndex = -1;
		bool success = false;

		try {
			if (avformat_open_input(&inFmtCtx, inputPath.c_str(), nullptr, nullptr) < 0) {
				throw std::runtime_error("Failed to open the input file.");
			}
			if (avformat_find_stream_info(inFmtCtx, nullptr) < 0) {
				throw std::runtime_error("Failed to retrieve stream information.");
			}

			for (unsigned int i = 0; i < inFmtCtx->nb_streams; i++) {
				if (inFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					videoStreamIndex = i;
					break;
				}
			}
			if (videoStreamIndex == -1) {
				throw std::runtime_error("No video stream found in the input file.");
			}

			AVCodecParameters* inCodecPar = inFmtCtx->streams[videoStreamIndex]->codecpar;
			const AVCodec* decoder = avcodec_find_decoder(inCodecPar->codec_id);
			if (!decoder) {
				throw std::runtime_error("No suitable decoder found.");
			}

			decCtx = avcodec_alloc_context3(decoder);
			if (!decCtx || avcodec_parameters_to_context(decCtx, inCodecPar) < 0 || avcodec_open2(decCtx, decoder, nullptr) < 0) {
				throw std::runtime_error("Failed to initialize the decoder context.");
			}

			auto [screenWidth, screenHeight] = physical_screen_size();

			if (avformat_alloc_output_context2(&outFmtCtx, nullptr, "mp4", outputPath.c_str()) < 0) {
				throw std::runtime_error("Failed to create the output context.");
			}

			const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
			if (!encoder) {
				throw std::runtime_error("H.264 encoder not found.");
			}

			AVStream* outStream = avformat_new_stream(outFmtCtx, nullptr);
			if (!outStream) {
				throw std::runtime_error("Failed to create the output stream.");
			}

			encCtx = avcodec_alloc_context3(encoder);
			if (!encCtx) {
				throw std::runtime_error("Failed to allocate the encoder context.");
			}

			encCtx->height = screenHeight;
			encCtx->width = screenWidth;
			encCtx->sample_aspect_ratio = AVRational{ 1, 1 };
			encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
			AVRational fps = inFmtCtx->streams[videoStreamIndex]->avg_frame_rate;
			if (fps.num == 0 || fps.den == 0) {
				fps = inFmtCtx->streams[videoStreamIndex]->r_frame_rate;
			}
			if (fps.num == 0 || fps.den == 0) {
				fps = { 30, 1 };
			}

			encCtx->framerate = fps;
			encCtx->time_base = av_inv_q(fps);
			int gopSize = static_cast<int>(std::lround(av_q2d(fps) * 10.0));
			encCtx->gop_size = gopSize > 0 ? gopSize : 250;

			if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
				encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}

			av_opt_set(encCtx->priv_data, "preset", "fast", 0);
			av_opt_set(encCtx->priv_data, "crf", "23", 0);

			if (avcodec_open2(encCtx, encoder, nullptr) < 0 || avcodec_parameters_from_context(outStream->codecpar, encCtx) < 0) {
				throw std::runtime_error("Failed to open or configure the encoder.");
			}

			if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
				if (avio_open(&outFmtCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
					throw std::runtime_error("Failed to create the output file for writing.");
				}
			}

			if (avformat_write_header(outFmtCtx, nullptr) < 0) {
				throw std::runtime_error("Failed to write the output file header.");
			}

			swsCtx = sws_getContext(
				decCtx->width, decCtx->height, decCtx->pix_fmt,
				encCtx->width, encCtx->height, encCtx->pix_fmt,
				SWS_BILINEAR, nullptr, nullptr, nullptr
			);
			if (!swsCtx) {
				throw std::runtime_error("Failed to initialize SwsContext.");
			}

			inPacket = av_packet_alloc();
			outPacket = av_packet_alloc();
			decFrame = av_frame_alloc();
			encFrame = av_frame_alloc();

			encFrame->format = encCtx->pix_fmt;
			encFrame->width = encCtx->width;
			encFrame->height = encCtx->height;
			if (av_frame_get_buffer(encFrame, 0) < 0) {
				throw std::runtime_error("Failed to allocate the output frame buffer.");
			}

			int64_t ptsCounter = 0;

			while (av_read_frame(inFmtCtx, inPacket) >= 0) {
				if (cancelled.load()) {
					break;
				}

				if (inPacket->stream_index == videoStreamIndex) {
					if (avcodec_send_packet(decCtx, inPacket) >= 0) {
						while (avcodec_receive_frame(decCtx, decFrame) >= 0) {
							if (cancelled.load()) break;

							av_frame_make_writable(encFrame);

							sws_scale(
								swsCtx, decFrame->data, decFrame->linesize, 0, decCtx->height,
								encFrame->data, encFrame->linesize
							);

							encFrame->pts = ptsCounter++;

							if (avcodec_send_frame(encCtx, encFrame) >= 0) {
								while (avcodec_receive_packet(encCtx, outPacket) >= 0) {
									av_packet_rescale_ts(outPacket, encCtx->time_base, outStream->time_base);
									outPacket->stream_index = outStream->index;
									av_interleaved_write_frame(outFmtCtx, outPacket);
									av_packet_unref(outPacket);
								}
							}
							av_frame_unref(decFrame);
						}
					}
				}
				av_packet_unref(inPacket);
			}

			if (!cancelled.load()) {
				avcodec_send_frame(encCtx, nullptr);
				while (avcodec_receive_packet(encCtx, outPacket) >= 0) {
					av_packet_rescale_ts(outPacket, encCtx->time_base, outStream->time_base);
					outPacket->stream_index = outStream->index;
					av_interleaved_write_frame(outFmtCtx, outPacket);
					av_packet_unref(outPacket);
				}
				av_write_trailer(outFmtCtx);
				success = true;
			}

		}
		catch (...) {
			if (inPacket) av_packet_free(&inPacket);
			if (outPacket) av_packet_free(&outPacket);
			if (decFrame) av_frame_free(&decFrame);
			if (encFrame) av_frame_free(&encFrame);
			if (swsCtx) sws_freeContext(swsCtx);
			if (decCtx) avcodec_free_context(&decCtx);
			if (encCtx) avcodec_free_context(&encCtx);
			if (inFmtCtx) avformat_close_input(&inFmtCtx);
			if (outFmtCtx) {
				if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE) && outFmtCtx->pb) {
					avio_closep(&outFmtCtx->pb);
				}
				avformat_free_context(outFmtCtx);
			}
			throw;
		}

		av_packet_free(&inPacket);
		av_packet_free(&outPacket);
		av_frame_free(&decFrame);
		av_frame_free(&encFrame);
		if (swsCtx) sws_freeContext(swsCtx);
		avcodec_free_context(&decCtx);
		avcodec_free_context(&encCtx);
		avformat_close_input(&inFmtCtx);
		if (outFmtCtx) {
			if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE) && outFmtCtx->pb) {
				avio_closep(&outFmtCtx->pb);
			}
			avformat_free_context(outFmtCtx);
		}

		if (cancelled.load() || !success) {
			throw std::runtime_error("Transcoding was interrupted or cancelled.");
		}
	}
}

namespace sdl {
	struct TextureDeleter {
		void operator()(SDL_Texture* p) const { SDL_DestroyTexture(p); }
	};
	using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

	struct RendererDeleter {
		void operator()(SDL_Renderer* p) const { SDL_DestroyRenderer(p); }
	};
	using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;

	struct WindowDeleter {
		void operator()(SDL_Window* p) const { SDL_DestroyWindow(p); }
	};
	using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
}

class Renderer {
public:
	Renderer(HWND hwnd, int w, int h) {
		LOG_INFO("Creating SDL renderer for HWND=" << hwnd << " size=" << w << "x" << h);
		SDL_PropertiesID props = SDL_CreateProperties();
		SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, hwnd);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
		window_.reset(SDL_CreateWindowWithProperties(props));
		SDL_DestroyProperties(props);

		if (!window_) {
			LOG_ERROR("SDL_CreateWindowWithProperties failed: " << SDL_GetError());
			throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
		}
		LOG_INFO("SDL Window created");

		renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
		if (!renderer_) {
			LOG_ERROR("SDL_CreateRenderer failed: " << SDL_GetError());
			throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
		}
		LOG_INFO("SDL Renderer created");
	}

	void run(FrameQueue& queue, std::atomic<bool>& cancelled,
		std::atomic<bool>& decoder_done) {
		LOG_INFO("Renderer main loop started");
		sdl::TexturePtr texture;

		double perf_freq = static_cast<double>(SDL_GetPerformanceFrequency());
		double pts_origin = -1.0;
		double wall_origin = 0.0;
		double last_pts = 0.0;
		bool   first_frame = true;
		int    frame_count = 0;

		while (!cancelled) {
			pump_events(cancelled);

			YUVFrame frame;
			if (!queue.pop(frame, cancelled, decoder_done)) {
				LOG_INFO("Renderer: queue pop returned false, exiting loop");
				break;
			}

			if (!first_frame && frame.pts_seconds < last_pts - 0.5) {
				pts_origin = frame.pts_seconds;
				wall_origin = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq;
			}
			last_pts = frame.pts_seconds;

			if (first_frame) {
				pts_origin = frame.pts_seconds;
				wall_origin = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq;
				texture = create_texture(frame.width, frame.height);
				first_frame = false;
			}

			double elapsed = static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq - wall_origin;
			double frame_time = frame.pts_seconds - pts_origin;
			double wait_sec = frame_time - elapsed;

			if (wait_sec > 0.002) {
				SDL_Delay(static_cast<Uint32>((wait_sec - 0.002) * 1000.0));
			}
			else if (wait_sec < -0.01) {
				if (frame_time > 0.1) {
					LOG_WARN("Frame late by " << -wait_sec << " sec");
				}
			}

			while ((static_cast<double>(SDL_GetPerformanceCounter()) / perf_freq - wall_origin) < frame_time) {
				std::this_thread::yield();
			}

			upload_and_present(texture.get(), frame);
			frame_count++;
		}
		LOG_INFO("Renderer finished, total frames rendered: " << frame_count);
	}

private:
	sdl::TexturePtr create_texture(int w, int h) {
		SDL_Texture* raw = SDL_CreateTexture(renderer_.get(),
			SDL_PIXELFORMAT_IYUV,
			SDL_TEXTUREACCESS_STREAMING,
			w, h);
		if (!raw) {
			LOG_ERROR("SDL_CreateTexture failed: " << SDL_GetError());
			throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
		}
		LOG_INFO("Texture created: " << w << "x" << h);
		return sdl::TexturePtr(raw);
	}

	void upload_and_present(SDL_Texture* tex, const YUVFrame& frame) {
		const AVFrame* f = frame.frame.get();

		if (!f->data[0] || !f->data[1] || !f->data[2]) {
			LOG_WARN("upload_and_present: frame has null plane data, skipping");
			return;
		}

		if (!SDL_UpdateYUVTexture(tex, nullptr,
			f->data[0], f->linesize[0],
			f->data[1], f->linesize[1],
			f->data[2], f->linesize[2])) {
			LOG_ERROR("SDL_UpdateYUVTexture failed: " << SDL_GetError());
			return;
		}

		if (!SDL_RenderTexture(renderer_.get(), tex, nullptr, nullptr)) {
			LOG_ERROR("SDL_RenderTexture failed: " << SDL_GetError());
			return;
		}

		if (!SDL_RenderPresent(renderer_.get())) {
			LOG_ERROR("SDL_RenderPresent failed: " << SDL_GetError());
		}
	}

	static void pump_events(std::atomic<bool>& cancelled) {
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				LOG_INFO("WM_QUIT received, cancelling");
				cancelled = true;
				return;
			}
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) {
				LOG_INFO("SDL_EVENT_QUIT received, cancelling");
				cancelled = true;
				return;
			}
		}
	}

	sdl::WindowPtr   window_;
	sdl::RendererPtr renderer_;
};

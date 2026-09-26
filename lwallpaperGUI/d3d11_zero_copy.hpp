#pragma once

// Zero-copy D3D11 rendering path for D3D11VA-decoded NV12 frames.
//
// Why this exists: SDL3's SDL_CreateTextureWithProperties can technically wrap
// an external ID3D11Texture2D* via SDL_PROP_TEXTURE_CREATE_D3D11_TEXTURE_POINTER,
// but only as a plain 2D texture. FFmpeg's D3D11VA decoder normally hands back
// frames backed by a slice of an *array* texture (AVFrame.data[0] = the array
// ID3D11Texture2D*, AVFrame.data[1] = the array slice index as intptr_t), and
// there's no public SDL API to bind a specific array slice when wrapping. So a
// literal wrap through SDL either binds the wrong slice or fails outright.
//
// This module sidesteps that by owning a minimal D3D11 render pipeline of its
// own: a device/context/swapchain created here (and handed to FFmpeg's hwaccel
// so decode and render share one device, no cross-device copy), plus a tiny
// pixel shader that samples the Y and UV planes of the correct array slice
// directly and writes RGB to the swapchain back buffer. No frame data ever
// touches system memory.
//
// Every initialization step can fail (old GPU, driver quirk, feature level too
// low, shader compile issue). On any failure this class leaves itself in a
// clearly non-functional state (is_valid() == false) and every method becomes
// a safe no-op, so the caller can fall back to the existing SDL+NV12/YUV420P
// CPU path without any special-casing beyond checking is_valid() once.

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/frame.h>
}

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace wallpaper {

	// BT.709 limited-range NV12 -> full-range RGB. Matches libx264's default
	// colorspace for HD content, which is what this project's transcoder produces.
	inline const char* kNv12ToRgbShaderSrc = R"(
Texture2D<float>  YPlane  : register(t0);
Texture2D<float2> UVPlane : register(t1);
SamplerState      Samp    : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    // Fullscreen triangle, no vertex buffer needed.
    VSOut o;
    float2 pos = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = float2(pos.x * 0.5 + 0.5, 1.0 - (pos.y * 0.5 + 0.5));
    return o;
}

float4 PSMain(VSOut input) : SV_TARGET {
    float y = YPlane.Sample(Samp, input.uv);
    float2 uv = UVPlane.Sample(Samp, input.uv) - float2(0.5, 0.5);

    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float u = uv.x * (255.0 / 224.0);
    float v = uv.y * (255.0 / 224.0);

    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;

    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";

	class D3D11ZeroCopyRenderer {
	public:
		D3D11ZeroCopyRenderer() = default;
		~D3D11ZeroCopyRenderer() { destroy(); }

		D3D11ZeroCopyRenderer(const D3D11ZeroCopyRenderer&) = delete;
		D3D11ZeroCopyRenderer& operator=(const D3D11ZeroCopyRenderer&) = delete;

		// Creates the device, swapchain (on hwnd, size w x h) and shader pipeline.
		// Returns true only if every step succeeded; on false the object is fully
		// torn down and is_valid() reports false, so the caller falls back safely.
		bool initialize(HWND hwnd, int w, int h) {
			if (!create_device()) { last_error_ = "device creation failed: " + last_error_; return false; }
			if (!create_swapchain(hwnd, w, h)) { last_error_ = "swapchain creation failed: " + last_error_; destroy(); return false; }
			if (!create_shaders()) { last_error_ = "shader creation failed: " + last_error_; destroy(); return false; }
			if (!create_sampler()) { last_error_ = "sampler creation failed: " + last_error_; destroy(); return false; }
			width_ = w;
			height_ = h;
			valid_ = true;
			return true;
		}

		bool is_valid() const { return valid_; }
		const std::string& last_error() const { return last_error_; }

		// Must be called once, after the decoder has confirmed zero-copy is active
		// (its lock/unlock/lock_ctx are only valid then). These are FFmpeg's own
		// D3D11VA synchronization primitives (documented default: an internal
		// mutex when left unset) - present_frame takes this same lock around any
		// direct D3D11 call that touches a decoder-owned surface, so it can't run
		// concurrently with the decoder's own internal writes to the same pool.
		// If never called (or passed all-null), present_frame proceeds unlocked -
		// which is only safe if the caller has some other guarantee decode and
		// present never overlap; the intended caller (run_zero_copy_loop) always
		// calls this first, so that fallback exists only to keep present_frame
		// from crashing if it's ever misused, not as a recommended mode.
		void set_sync_callbacks(void (*lock)(void*), void (*unlock)(void*), void* lock_ctx) {
			hw_lock_ = lock;
			hw_unlock_ = unlock;
			hw_lock_ctx_ = lock_ctx;
		}

		// The device FFmpeg's D3D11VA hwaccel should be told to use, so decoded
		// surfaces live on the same device this renderer draws from (no cross-
		// device copy). Call this before av_hwdevice_ctx_create-equivalent setup.
		ID3D11Device* device() const { return device_.Get(); }

		// Presents the given decoded D3D11 frame directly: frame->data[0] is the
		// array ID3D11Texture2D*, frame->data[1] is the array slice index. Returns
		// false (without throwing) if anything about the frame or a GPU call is
		// unexpected - the caller should treat that as "this frame failed, try
		// the next one" rather than a fatal error, since a single bad frame
		// shouldn't take down playback.
		bool present_frame(const AVFrame* frame) {
			if (!valid_ || !frame || frame->format != AV_PIX_FMT_D3D11) return false;

			auto* array_texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
			auto slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
			if (!array_texture) return false;

			D3D11_TEXTURE2D_DESC array_desc{};
			array_texture->GetDesc(&array_desc);

			if (!logged_format_once_) {
				LOG_INFO("Zero-copy: decoder array texture format=" << static_cast<int>(array_desc.Format)
					<< " size=" << array_desc.Width << "x" << array_desc.Height
					<< " arraySize=" << array_desc.ArraySize << " bindFlags=" << array_desc.BindFlags);
				logged_format_once_ = true;
			}

			if (!ensure_slice_copy_texture(array_desc)) return false;

			// Base D3D11_TEX2D_ARRAY_SRV (via ID3D11Device::CreateShaderResourceView)
			// has no PlaneSlice field - that only exists on D3D11_TEX2D_ARRAY_SRV1,
			// which needs ID3D11Device3::CreateShaderResourceView1 (D3D11.3). Rather
			// than require that feature level, copy just this one array slice into a
			// small single-layer texture (a GPU-to-GPU copy, no CPU involved, and
			// far cheaper than the CPU transfer this whole path exists to avoid),
			// then build ordinary non-array Y/UV SRVs against that - format alone
			// selects the plane there, which the base API does support directly.
			UINT src_subresource = D3D11CalcSubresource(0, slice, 1);
			if (hw_lock_) hw_lock_(hw_lock_ctx_);
			context_->CopySubresourceRegion(slice_copy_.Get(), 0, 0, 0, 0,
				array_texture, src_subresource, nullptr);
			if (hw_unlock_) hw_unlock_(hw_lock_ctx_);

			ComPtr<ID3D11ShaderResourceView> y_srv, uv_srv;
			if (!make_plane_srv(slice_copy_.Get(), DXGI_FORMAT_R8_UNORM, y_srv)) return false;
			if (!make_plane_srv(slice_copy_.Get(), DXGI_FORMAT_R8G8_UNORM, uv_srv)) return false;

			ID3D11ShaderResourceView* srvs[2] = { y_srv.Get(), uv_srv.Get() };
			context_->PSSetShaderResources(0, 2, srvs);
			context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

			ID3D11RenderTargetView* rtv = back_buffer_rtv_.Get();
			context_->OMSetRenderTargets(1, &rtv, nullptr);

			const float debug_red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
			context_->ClearRenderTargetView(rtv, debug_red);

			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(width_);
			vp.Height = static_cast<float>(height_);
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			context_->RSSetViewports(1, &vp);

			context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
			context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
			context_->Draw(3, 0);

			// Unbind the SRVs before Present so the next frame's CreateShaderResourceView
			// on a texture that might otherwise still be bound doesn't trigger a D3D
			// debug-layer warning (and, on some drivers, a stall) about a resource
			// being simultaneously bound as input and pending write.
			ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
			context_->PSSetShaderResources(0, 2, null_srvs);

			LOG_INFO("Zero-copy: about to Present");
			// SyncInterval=0: SyncInterval=1 waits for a vblank/composition signal
			// from DWM, but this swapchain lives on a WorkerW-child window behind
			// the desktop, outside DWM's normal composited presentation path - it
			// may never receive that signal, which is what caused Present to hang
			// indefinitely in testing. Frame pacing is already handled by the
			// pts-based wait loop in run_zero_copy_loop, so presenting immediately
			// here doesn't risk flooding the GPU with uncapped frames.
			HRESULT hr = swapchain_->Present(0, 0);
			LOG_INFO("Zero-copy: Present done, hr=" << std::to_string(static_cast<long>(hr)));
			if (FAILED(hr)) {
				last_error_ = "Present failed, hr=" + std::to_string(static_cast<long>(hr));
				// A lost device is the one failure we don't try to recover from inline;
				// the caller should stop using this renderer and fall back.
				if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
					valid_ = false;
				}
				return false;
			}
			return true;
		}

		void destroy() {
			valid_ = false;
			sampler_.Reset();
			pixel_shader_.Reset();
			vertex_shader_.Reset();
			back_buffer_rtv_.Reset();
			swapchain_.Reset();
			context_.Reset();
			device_.Reset();
		}

	private:
		bool create_device() {
			D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			D3D_FEATURE_LEVEL got{};
			ComPtr<ID3D11Device> base_device;
			ComPtr<ID3D11DeviceContext> base_context;

			HRESULT hr = D3D11CreateDevice(
				nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT,
				levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
				&base_device, &got, &base_context);
			if (FAILED(hr)) {
				last_error_ = "D3D11CreateDevice hr=" + std::to_string(static_cast<long>(hr));
				return false;
			}

			hr = base_device.As(&device_);
			if (FAILED(hr)) { last_error_ = "ID3D11Device1 query failed"; return false; }
			hr = base_context.As(&context_);
			if (FAILED(hr)) { last_error_ = "ID3D11DeviceContext1 query failed"; return false; }

			return true;
		}

		bool create_swapchain(HWND hwnd, int w, int h) {
			ComPtr<IDXGIDevice> dxgi_device;
			if (FAILED(device_.As(&dxgi_device))) { last_error_ = "IDXGIDevice query failed"; return false; }

			ComPtr<IDXGIAdapter> adapter;
			if (FAILED(dxgi_device->GetAdapter(&adapter))) { last_error_ = "GetAdapter failed"; return false; }

			ComPtr<IDXGIFactory2> factory;
			if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { last_error_ = "GetParent(IDXGIFactory2) failed"; return false; }

			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = static_cast<UINT>(w);
			desc.Height = static_cast<UINT>(h);
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 1;
			desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

			HRESULT hr = factory->CreateSwapChainForHwnd(
				device_.Get(), hwnd, &desc, nullptr, nullptr, &swapchain_);
			if (FAILED(hr)) {
				last_error_ = "CreateSwapChainForHwnd hr=" + std::to_string(static_cast<long>(hr));
				return false;
			}

			ComPtr<ID3D11Texture2D> back_buffer;
			if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
				last_error_ = "swapchain GetBuffer failed";
				return false;
			}
			if (FAILED(device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &back_buffer_rtv_))) {
				last_error_ = "CreateRenderTargetView failed";
				return false;
			}
			return true;
		}

		bool create_shaders() {
			ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
			UINT flags = 0;

			HRESULT hr = D3DCompile(kNv12ToRgbShaderSrc, strlen(kNv12ToRgbShaderSrc),
				"nv12_to_rgb", nullptr, nullptr, "VSMain", "vs_4_0", flags, 0,
				&vs_blob, &err_blob);
			if (FAILED(hr)) {
				last_error_ = "VS compile failed: ";
				if (err_blob) last_error_ += static_cast<const char*>(err_blob->GetBufferPointer());
				return false;
			}

			err_blob.Reset();
			hr = D3DCompile(kNv12ToRgbShaderSrc, strlen(kNv12ToRgbShaderSrc),
				"nv12_to_rgb", nullptr, nullptr, "PSMain", "ps_4_0", flags, 0,
				&ps_blob, &err_blob);
			if (FAILED(hr)) {
				last_error_ = "PS compile failed: ";
				if (err_blob) last_error_ += static_cast<const char*>(err_blob->GetBufferPointer());
				return false;
			}

			if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_))) {
				last_error_ = "CreateVertexShader failed";
				return false;
			}
			if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader_))) {
				last_error_ = "CreatePixelShader failed";
				return false;
			}
			return true;
		}

		bool create_sampler() {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device_->CreateSamplerState(&desc, &sampler_))) {
				last_error_ = "CreateSamplerState failed";
				return false;
			}
			return true;
		}

		// Builds an SRV for one plane (Y or UV, selected by `format`) of a plain
		// (non-array) NV12 texture. Format alone selects the plane here - this is
		// the documented approach for non-array NV12 textures and is what makes
		// the intermediate slice-copy in present_frame necessary in the first
		// place (the decoder's original texture is an array, which the base D3D11
		// SRV API can't plane-select into directly - see present_frame).
		bool make_plane_srv(ID3D11Texture2D* texture, DXGI_FORMAT format,
			ComPtr<ID3D11ShaderResourceView>& out) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = format;
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MostDetailedMip = 0;
			desc.Texture2D.MipLevels = 1;

			HRESULT hr = device_->CreateShaderResourceView(texture, &desc, &out);
			if (FAILED(hr)) {
				last_error_ = "CreateShaderResourceView (plane, format " +
					std::to_string(static_cast<int>(format)) +
					") hr=" + std::to_string(static_cast<long>(hr));
				return false;
			}
			return true;
		}

		// Lazily (re)creates the single-slice copy target used as a stepping stone
		// between the decoder's array texture and plane-specific SRVs. Only
		// recreated if the source format/size actually changes (shouldn't happen
		// mid-stream for one video, but a defensive check costs nothing here).
		bool ensure_slice_copy_texture(const D3D11_TEXTURE2D_DESC& src_desc) {
			if (slice_copy_ && slice_copy_w_ == src_desc.Width && slice_copy_h_ == src_desc.Height
				&& slice_copy_fmt_ == src_desc.Format) {
				return true;
			}

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = src_desc.Width;
			desc.Height = src_desc.Height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = src_desc.Format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			ComPtr<ID3D11Texture2D> tex;
			HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &tex);
			if (FAILED(hr)) {
				last_error_ = "CreateTexture2D (slice copy target) hr=" + std::to_string(static_cast<long>(hr));
				return false;
			}

			slice_copy_ = tex;
			slice_copy_w_ = src_desc.Width;
			slice_copy_h_ = src_desc.Height;
			slice_copy_fmt_ = src_desc.Format;
			return true;
		}

		ComPtr<ID3D11Device1>        device_;
		ComPtr<ID3D11DeviceContext1> context_;
		ComPtr<IDXGISwapChain1>      swapchain_;
		ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
		ComPtr<ID3D11VertexShader>   vertex_shader_;
		ComPtr<ID3D11PixelShader>    pixel_shader_;
		ComPtr<ID3D11SamplerState>   sampler_;
		ComPtr<ID3D11Texture2D>      slice_copy_;
		UINT       slice_copy_w_ = 0;
		UINT       slice_copy_h_ = 0;
		DXGI_FORMAT slice_copy_fmt_ = DXGI_FORMAT_UNKNOWN;

		void (*hw_lock_)(void*) = nullptr;
		void (*hw_unlock_)(void*) = nullptr;
		void* hw_lock_ctx_ = nullptr;

		int width_ = 0;
		int height_ = 0;
		bool valid_ = false;
		bool logged_format_once_ = false;
		std::string last_error_;
	};

} // namespace wallpaper
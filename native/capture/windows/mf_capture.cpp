// Windows Media Foundation capture backend.
//
// The backend is deliberately thin: it enumerates devices, selects an explicit
// media type, and delivers samples from an async IMFSourceReader. Negotiation,
// timestamp mapping, the watchdog, reconnect and the CAP codes live in the
// platform-neutral CameraSource, so the same state machine is tested on every
// platform with a fake backend.
//
// No advanced video processing is enabled. MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING
// and MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING silently insert converters and
// add latency; the requested NV12/YUY2/BGRA8 type is set explicitly instead,
// and a rejection steps down the preference list.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "avatar/capture/camera_device.h"

namespace avatar::capture {

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

std::string hresult_text(HRESULT result) {
    const auto value = static_cast<std::uint32_t>(result);
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text = "hresult=0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> static_cast<unsigned>(shift)) & 0xFu]);
    }
    return text;
}

std::string to_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::vector<char> buffer(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer.data(), size, nullptr, nullptr);
    return std::string(buffer.data());
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer.data(),
                        size);
    return buffer;
}

HRESULT ensure_com_apartment() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return result == RPC_E_CHANGED_MODE ? S_OK : result;
}

HRESULT ensure_media_foundation() {
    static const HRESULT result = [] {
        const HRESULT com = ensure_com_apartment();
        if (FAILED(com)) {
            return com;
        }
        return MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }();
    return result;
}

struct CoTaskMemString {
    LPWSTR value = nullptr;
    CoTaskMemString() = default;
    CoTaskMemString(const CoTaskMemString&) = delete;
    CoTaskMemString& operator=(const CoTaskMemString&) = delete;
    ~CoTaskMemString() {
        if (value != nullptr) {
            CoTaskMemFree(value);
        }
    }
};

struct DeviceArray {
    IMFActivate** values = nullptr;
    UINT32 count = 0;
    DeviceArray() = default;
    DeviceArray(const DeviceArray&) = delete;
    DeviceArray& operator=(const DeviceArray&) = delete;
    ~DeviceArray() {
        for (UINT32 index = 0; index < count; ++index) {
            if (values[index] != nullptr) {
                values[index]->Release();
            }
        }
        if (values != nullptr) {
            CoTaskMemFree(values);
        }
    }
};

GUID subtype_for(PixelFormat format) {
    switch (format) {
        case PixelFormat::nv12:
            return MFVideoFormat_NV12;
        case PixelFormat::yuy2:
            return MFVideoFormat_YUY2;
        case PixelFormat::mjpeg:
            return MFVideoFormat_MJPG;
        case PixelFormat::bgra8:
            return MFVideoFormat_RGB32;
        case PixelFormat::rgba8:
            return MFVideoFormat_ARGB32;
    }
    return MFVideoFormat_NV12;
}

bool format_for_subtype(const GUID& subtype, PixelFormat& format) {
    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        format = PixelFormat::nv12;
        return true;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) {
        format = PixelFormat::yuy2;
        return true;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_MJPG)) {
        format = PixelFormat::mjpeg;
        return true;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_RGB32) ||
        IsEqualGUID(subtype, MFVideoFormat_ARGB32)) {
        format = PixelFormat::bgra8;
        return true;
    }
    return false;
}

std::uint32_t default_stride(const CameraMode& mode) {
    switch (mode.format) {
        case PixelFormat::nv12:
            return mode.width;
        case PixelFormat::yuy2:
            return mode.width * 2u;
        case PixelFormat::bgra8:
        case PixelFormat::rgba8:
            return mode.width * 4u;
        case PixelFormat::mjpeg:
            return 0;
    }
    return 0;
}

bool mode_from_media_type(IMFMediaType* type, CameraMode& mode) {
    if (type == nullptr) {
        return false;
    }
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return false;
    }
    PixelFormat format = PixelFormat::nv12;
    if (!format_for_subtype(subtype, format)) {
        return false;
    }
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) || width == 0 ||
        height == 0) {
        return false;
    }
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) ||
        numerator == 0 || denominator == 0) {
        numerator = 30;
        denominator = 1;
    }
    mode.width = width;
    mode.height = height;
    mode.fps_numerator = numerator;
    mode.fps_denominator = denominator;
    mode.format = format;
    return true;
}

ColorSpace color_space_from_media_type(IMFMediaType* type, PixelFormat format,
                                       std::uint32_t height) {
    if (format == PixelFormat::bgra8 || format == PixelFormat::rgba8) {
        return ColorSpace::srgb;
    }
    UINT32 matrix = 0;
    const bool has_matrix =
        type != nullptr && SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix));
    UINT32 range = 0;
    const bool has_range =
        type != nullptr && SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range));
    const bool full = has_range && range == MFNominalRange_0_255;
    const bool is_bt709 =
        (has_matrix && matrix == MFVideoTransferMatrix_BT709) || (!has_matrix && height >= 720);
    if (is_bt709) {
        return full ? ColorSpace::bt709_full : ColorSpace::bt709_limited;
    }
    return ColorSpace::bt601_limited;
}

} // namespace

class MfDeviceBackend;

/// The async source-reader callback. It holds a raw owner pointer (not a COM
/// reference) so the backend can detach it before releasing the reader.
class SourceReaderCallback final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<
                                              Microsoft::WRL::ClassicCom>,
                                          IMFSourceReaderCallback> {
public:
    explicit SourceReaderCallback(MfDeviceBackend* owner) noexcept : owner_(owner) {}

    void detach() noexcept { owner_.store(nullptr, std::memory_order_release); }

    STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_flags, DWORD stream_index,
                              LONGLONG timestamp, IMFSample* sample) override;

    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    std::atomic<MfDeviceBackend*> owner_;
};

class MfDeviceBackend final : public ICameraDeviceBackend {
public:
    MfDeviceBackend() = default;
    ~MfDeviceBackend() override { stop(); }

    MfDeviceBackend(const MfDeviceBackend&) = delete;
    MfDeviceBackend& operator=(const MfDeviceBackend&) = delete;

    core::Status enumerate(std::vector<CameraDevice>& devices) override {
        devices.clear();
        const HRESULT init = ensure_media_foundation();
        if (FAILED(init)) {
            return core::Status::error(core::StatusCode::internal_error,
                                       "Media Foundation is unavailable", hresult_text(init));
        }

        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(attributes.GetAddressOf(), 1);
        if (FAILED(result)) {
            return core::Status::error(core::StatusCode::internal_error,
                                       "cannot create device attributes", hresult_text(result));
        }
        result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(result)) {
            return core::Status::error(core::StatusCode::internal_error,
                                       "cannot configure device enumeration",
                                       hresult_text(result));
        }

        DeviceArray array;
        result = MFEnumDeviceSources(attributes.Get(), &array.values, &array.count);
        if (FAILED(result)) {
            return core::Status::error(core::StatusCode::internal_error,
                                       "cannot enumerate capture devices", hresult_text(result));
        }
        for (UINT32 index = 0; index < array.count; ++index) {
            CoTaskMemString link;
            if (FAILED(array.values[index]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link.value,
                    nullptr)) ||
                link.value == nullptr) {
                continue;
            }
            CoTaskMemString name;
            const bool has_name = SUCCEEDED(array.values[index]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name.value, nullptr));
            CameraDevice device;
            device.id = to_utf8(link.value);
            device.name = has_name ? to_utf8(name.value) : device.id;
            if (!device.id.empty()) {
                devices.push_back(std::move(device));
            }
        }
        return core::Status::success();
    }

    CameraFailure capabilities(std::string_view device_id, CameraCapabilities& out) override {
        out.modes.clear();
        const HRESULT init = ensure_media_foundation();
        if (FAILED(init)) {
            return fail(init, "Media Foundation is unavailable");
        }

        ComPtr<IMFActivate> activate;
        CameraFailure failure = acquire_activate(device_id, activate);
        if (!failure.ok()) {
            return failure;
        }

        ComPtr<IMFMediaSource> source;
        HRESULT result = activate->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
        if (FAILED(result)) {
            activate->ShutdownObject();
            return fail_reader_open(result, "opening the camera");
        }
        struct SourceGuard {
            IMFMediaSource* source = nullptr;
            IMFActivate* activate = nullptr;
            ~SourceGuard() {
                if (source != nullptr) {
                    source->Shutdown();
                }
                if (activate != nullptr) {
                    activate->ShutdownObject();
                }
            }
        } guard{source.Get(), activate.Get()};

        ComPtr<IMFPresentationDescriptor> descriptor;
        result = source->CreatePresentationDescriptor(descriptor.GetAddressOf());
        if (FAILED(result)) {
            return fail(result, "reading the camera description");
        }

        DWORD stream_count = 0;
        result = descriptor->GetStreamDescriptorCount(&stream_count);
        if (FAILED(result)) {
            return fail(result, "reading the camera streams");
        }
        for (DWORD stream_index = 0; stream_index < stream_count; ++stream_index) {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> stream;
            if (FAILED(descriptor->GetStreamDescriptorByIndex(stream_index, &selected,
                                                             stream.GetAddressOf()))) {
                continue;
            }
            ComPtr<IMFMediaTypeHandler> handler;
            if (FAILED(stream->GetMediaTypeHandler(handler.GetAddressOf()))) {
                continue;
            }
            GUID major{};
            if (FAILED(handler->GetMajorType(&major)) || !IsEqualGUID(major, MFMediaType_Video)) {
                continue;
            }
            DWORD type_count = 0;
            if (FAILED(handler->GetMediaTypeCount(&type_count))) {
                continue;
            }
            for (DWORD type_index = 0; type_index < type_count; ++type_index) {
                ComPtr<IMFMediaType> type;
                if (FAILED(handler->GetMediaTypeByIndex(type_index, type.GetAddressOf()))) {
                    continue;
                }
                CameraMode mode;
                if (!mode_from_media_type(type.Get(), mode)) {
                    continue;
                }
                const bool duplicate =
                    std::any_of(out.modes.begin(), out.modes.end(),
                                [&mode](const CameraMode& candidate) { return candidate == mode; });
                if (!duplicate) {
                    out.modes.push_back(mode);
                }
            }
        }
        return CameraFailure{};
    }

    CameraFailure open(std::string_view device_id, const CameraMode& mode) override {
        stop();
        const HRESULT init = ensure_media_foundation();
        if (FAILED(init)) {
            return fail(init, "Media Foundation is unavailable");
        }

        ComPtr<IMFActivate> activate;
        CameraFailure failure = acquire_activate(device_id, activate);
        if (!failure.ok()) {
            return failure;
        }

        ComPtr<IMFMediaSource> source;
        HRESULT result = activate->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
        if (FAILED(result)) {
            activate->ShutdownObject();
            return fail_reader_open(result, "opening the camera");
        }

        ComPtr<IMFAttributes> attributes;
        result = MFCreateAttributes(attributes.GetAddressOf(), 2);
        if (FAILED(result)) {
            source->Shutdown();
            activate->ShutdownObject();
            return fail(result, "creating source reader attributes");
        }

        ComPtr<SourceReaderCallback> callback = Microsoft::WRL::Make<SourceReaderCallback>(this);
        result = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        if (FAILED(result)) {
            source->Shutdown();
            activate->ShutdownObject();
            return fail(result, "configuring the asynchronous callback");
        }

        ComPtr<IMFSourceReader> reader;
        result = MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(),
                                                     reader.GetAddressOf());
        if (FAILED(result)) {
            source->Shutdown();
            activate->ShutdownObject();
            return fail_reader_open(result, "creating the source reader");
        }

        // Explicit media type: no MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
        // no D3D manager. A rejection is a step-down, not a conversion.
        ComPtr<IMFMediaType> type;
        result = MFCreateMediaType(type.GetAddressOf());
        if (FAILED(result)) {
            reader.Reset();
            source->Shutdown();
            activate->ShutdownObject();
            return fail(result, "creating the media type");
        }
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, subtype_for(mode.format));
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, mode.width, mode.height);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, mode.fps_numerator,
                            mode.fps_denominator);
        MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        result = reader->SetStreamSelection(kFirstVideoStream, TRUE);
        if (FAILED(result)) {
            reader.Reset();
            source->Shutdown();
            activate->ShutdownObject();
            return fail(result, "selecting the video stream");
        }

        result = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.Get());
        if (FAILED(result)) {
            reader.Reset();
            source->Shutdown();
            activate->ShutdownObject();
            return fail(result, "selecting the capture format");
        }

        ComPtr<IMFMediaType> actual;
        CameraMode actual_mode = mode;
        if (SUCCEEDED(reader->GetCurrentMediaType(kFirstVideoStream, actual.GetAddressOf()))) {
            CameraMode reported;
            if (mode_from_media_type(actual.Get(), reported)) {
                actual_mode = reported;
            }
        }

        UINT32 raw_stride = 0;
        std::uint32_t stride = default_stride(actual_mode);
        if (actual != nullptr &&
            SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
            const auto signed_stride = static_cast<std::int32_t>(raw_stride);
            stride = static_cast<std::uint32_t>(signed_stride < 0 ? -signed_stride : signed_stride);
        }
        if (stride == 0) {
            stride = default_stride(actual_mode);
        }

        mode_ = actual_mode;
        color_space_ = color_space_from_media_type(actual.Get(), actual_mode.format,
                                                   actual_mode.height);
        stride_ = stride;
        reader_ = std::move(reader);
        source_ = std::move(source);
        activate_ = std::move(activate);
        callback_ = std::move(callback);
        return CameraFailure{};
    }

    CameraFailure start(const CameraBackendCallbacks& callbacks) override {
        if (!reader_) {
            CameraFailure failure;
            failure.reason = CameraFailureReason::internal;
            failure.code = core::StatusCode::internal_error;
            failure.message = "the capture device is not open";
            last_error_ = failure.message;
            return failure;
        }
        callbacks_ = callbacks;
        running_.store(true, std::memory_order_release);
        const HRESULT result =
            reader_->ReadSample(kFirstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result)) {
            running_.store(false);
            callbacks_ = {};
            return fail(result, "starting the capture stream");
        }
        return CameraFailure{};
    }

    void stop() noexcept override {
        running_.store(false, std::memory_order_release);
        {
            // Waits for an in-flight OnReadSample body to finish.
            const std::lock_guard<std::mutex> lock(callback_mutex_);
            callbacks_ = {};
        }
        if (callback_) {
            callback_->detach();
        }
        if (reader_) {
            reader_->Flush(kFirstVideoStream);
        }
        if (source_) {
            source_->Shutdown();
        }
        if (activate_) {
            activate_->ShutdownObject();
        }
        reader_.Reset();
        source_.Reset();
        activate_.Reset();
        callback_.Reset();
        mode_ = {};
        color_space_ = ColorSpace::srgb;
        stride_ = 0;
    }

    void close() noexcept override { stop(); }

    [[nodiscard]] bool running() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] CameraMode negotiated_mode() const noexcept override { return mode_; }

    [[nodiscard]] ColorSpace color_space() const noexcept override { return color_space_; }

    [[nodiscard]] std::string_view last_error() const noexcept override { return last_error_; }

private:
    friend class SourceReaderCallback;

    CameraFailure fail(HRESULT result, const char* stage) {
        CameraFailure failure;
        failure.hresult = static_cast<std::int32_t>(result);
        failure.reason = classify_camera_hresult(failure.hresult);
        failure.code = failure.reason == CameraFailureReason::invalid_format
                           ? core::StatusCode::unsupported
                           : core::StatusCode::internal_error;
        failure.message = std::string(stage) + " failed (" + hresult_text(result) + ")";
        if (failure.reason == CameraFailureReason::busy) {
            failure.message += "; the camera is likely held by Teams, Zoom, OBS or a browser";
        }
        last_error_ = failure.message;
        return failure;
    }

    /// A failure before the media type is set. Some drivers report exclusive
    /// access as MF_E_UNSUPPORTED_BYTESTREAM_TYPE, which would otherwise look
    /// like a format rejection and produce the wrong CAP code.
    CameraFailure fail_reader_open(HRESULT result, const char* stage) {
        CameraFailure failure = fail(result, stage);
        if (failure.reason == CameraFailureReason::invalid_format) {
            failure.reason = CameraFailureReason::busy;
            failure.code = core::StatusCode::internal_error;
            failure.message = std::string(stage) +
                              " failed; the camera is likely held by another application "
                              "(Teams, Zoom, OBS or a browser) (" +
                              hresult_text(result) + ")";
            last_error_ = failure.message;
        }
        return failure;
    }

    CameraFailure acquire_activate(std::string_view device_id, ComPtr<IMFActivate>& out) {
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(attributes.GetAddressOf(), 1);
        if (FAILED(result)) {
            return fail(result, "creating device attributes");
        }
        result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(result)) {
            return fail(result, "configuring device enumeration");
        }

        DeviceArray array;
        result = MFEnumDeviceSources(attributes.Get(), &array.values, &array.count);
        if (FAILED(result)) {
            return fail(result, "enumerating capture devices");
        }
        const std::wstring wanted = to_wide(device_id);
        for (UINT32 index = 0; index < array.count; ++index) {
            CoTaskMemString link;
            if (FAILED(array.values[index]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link.value,
                    nullptr)) ||
                link.value == nullptr) {
                continue;
            }
            if (wanted == link.value) {
                out = array.values[index];
                break;
            }
        }
        if (!out) {
            CameraFailure failure;
            failure.reason = CameraFailureReason::device_gone;
            failure.code = core::StatusCode::not_found;
            failure.message = "the camera is not present: " + std::string(device_id);
            last_error_ = failure.message;
            return failure;
        }
        return CameraFailure{};
    }

    void report_error(CameraBackendErrorKind kind, HRESULT result) noexcept {
        if (callbacks_.on_error != nullptr) {
            callbacks_.on_error(callbacks_.context, kind, static_cast<std::int32_t>(result));
        }
    }

    void on_read_sample(HRESULT status, DWORD stream_flags, LONGLONG timestamp,
                        IMFSample* sample) noexcept {
        const std::lock_guard<std::mutex> lock(callback_mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        if (FAILED(status) || (stream_flags & MF_SOURCE_READERF_ERROR) != 0 ||
            (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            running_.store(false, std::memory_order_release);
            const CameraFailureReason reason =
                classify_camera_hresult(static_cast<std::int32_t>(status));
            switch (reason) {
                case CameraFailureReason::busy:
                    report_error(CameraBackendErrorKind::busy, status);
                    break;
                case CameraFailureReason::permission_denied:
                    report_error(CameraBackendErrorKind::permission_denied, status);
                    break;
                case CameraFailureReason::device_gone:
                case CameraFailureReason::device_lost:
                    report_error(CameraBackendErrorKind::device_lost, status);
                    break;
                default:
                    report_error(CameraBackendErrorKind::read_error, status);
                    break;
            }
            return;
        }
        if ((stream_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            // Never publish a frame whose layout we did not negotiate.
            running_.store(false, std::memory_order_release);
            report_error(CameraBackendErrorKind::read_error, status);
            return;
        }

        if (sample != nullptr && callbacks_.on_sample != nullptr) {
            ComPtr<IMFMediaBuffer> buffer;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && data != nullptr) {
                    LONGLONG effective_timestamp = timestamp;
                    LONGLONG sample_time = 0;
                    if (SUCCEEDED(sample->GetSampleTime(&sample_time))) {
                        effective_timestamp = sample_time;
                    }
                    CameraSample out;
                    out.data = data;
                    out.size = length;
                    out.width = mode_.width;
                    out.height = mode_.height;
                    out.format = mode_.format;
                    out.color_space = color_space_;
                    fill_plane_layout(out, length);
                    if (effective_timestamp >= 0) {
                        out.source_timestamp_ns =
                            static_cast<std::uint64_t>(effective_timestamp) * 100ull;
                        out.has_source_timestamp = true;
                    }
                    callbacks_.on_sample(callbacks_.context, out);
                    buffer->Unlock();
                }
            }
        }

        if (running_.load(std::memory_order_acquire) && reader_) {
            const HRESULT result =
                reader_->ReadSample(kFirstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(result)) {
                running_.store(false, std::memory_order_release);
                report_error(CameraBackendErrorKind::read_error, result);
            }
        }
    }

    void fill_plane_layout(CameraSample& sample, std::size_t length) const noexcept {
        const std::uint32_t stride = stride_;
        const std::uint32_t height = mode_.height;
        if (stride == 0 || height == 0) {
            return;
        }
        sample.strides[0] = stride;
        sample.plane_offsets[0] = 0;
        sample.plane_sizes[0] = std::min<std::size_t>(static_cast<std::size_t>(stride) * height,
                                                      length);
        if (mode_.format == PixelFormat::nv12) {
            const std::size_t luma = static_cast<std::size_t>(stride) * height;
            if (length > luma) {
                sample.strides[1] = stride;
                sample.plane_offsets[1] = luma;
                sample.plane_sizes[1] = std::min<std::size_t>(
                    static_cast<std::size_t>(stride) * (height / 2u), length - luma);
            }
        }
    }

    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFActivate> activate_;
    ComPtr<SourceReaderCallback> callback_;
    std::mutex callback_mutex_{};
    CameraBackendCallbacks callbacks_{};
    std::atomic<bool> running_{false};
    CameraMode mode_{};
    ColorSpace color_space_ = ColorSpace::srgb;
    std::uint32_t stride_ = 0;
    std::string last_error_{};
};

std::unique_ptr<ICameraDeviceBackend> create_platform_camera_backend() {
    return std::make_unique<MfDeviceBackend>();
}

STDMETHODIMP SourceReaderCallback::OnReadSample(HRESULT status, DWORD stream_flags,
                                                DWORD stream_index, LONGLONG timestamp,
                                                IMFSample* sample) {
    (void)stream_index;
    MfDeviceBackend* owner = owner_.load(std::memory_order_acquire);
    if (owner != nullptr) {
        owner->on_read_sample(status, stream_flags, timestamp, sample);
    }
    return S_OK;
}

} // namespace avatar::capture

#endif // _WIN32

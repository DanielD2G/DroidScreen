#include "win_volume_controller.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace droidscreen {

namespace {

struct ScopedComApartment {
  bool initialized = false;

  ScopedComApartment() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = SUCCEEDED(hr);
  }

  ~ScopedComApartment() {
    if (initialized) {
      CoUninitialize();
    }
  }
};

class VolumeCallback;

class VolumeImpl {
public:
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  Microsoft::WRL::ComPtr<IMMDevice> endpoint;
  Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume;
  Microsoft::WRL::ComPtr<VolumeCallback> callback;
  GUID event_context{};
  std::mutex mutex;
  WinVolumeController::StateCallback state_callback;
};

class VolumeCallback : public IAudioEndpointVolumeCallback {
public:
  explicit VolumeCallback(VolumeImpl *impl) : impl_(impl) {}

  STDMETHODIMP QueryInterface(REFIID riid, VOID **ppvInterface) override {
    if (!ppvInterface)
      return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ppvInterface = static_cast<IAudioEndpointVolumeCallback *>(this);
      AddRef();
      return S_OK;
    }
    *ppvInterface = nullptr;
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_count_; }

  STDMETHODIMP_(ULONG) Release() override {
    ULONG count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notify) override {
    if (!notify || !impl_)
      return S_OK;
    if (IsEqualGUID(notify->guidEventContext, impl_->event_context)) {
      return S_OK;
    }

    WinVolumeController::StateCallback callback;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      callback = impl_->state_callback;
    }

    if (callback) {
      uint16_t level =
          static_cast<uint16_t>(notify->fMasterVolume * 65535.0f + 0.5f);
      callback(level, notify->bMuted != FALSE);
    }
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_{1};
  VolumeImpl *impl_ = nullptr;
};

} // namespace

struct WinVolumeController::Impl : VolumeImpl {};

WinVolumeController::WinVolumeController() : impl_(std::make_unique<Impl>()) {}

WinVolumeController::~WinVolumeController() { shutdown(); }

bool WinVolumeController::init() {
  if (!impl_)
    return false;
  ScopedComApartment apartment;

  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&impl_->enumerator));
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] CoCreateInstance failed: 0x%08lx\n",
            (unsigned long)hr);
    return false;
  }

  hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                  &impl_->endpoint);
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] GetDefaultAudioEndpoint failed: 0x%08lx\n",
            (unsigned long)hr);
    return false;
  }

  hr = impl_->endpoint->Activate(
      __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
      reinterpret_cast<void **>(impl_->volume.GetAddressOf()));
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] Activate IAudioEndpointVolume failed: 0x%08lx\n",
            (unsigned long)hr);
    return false;
  }

  CoCreateGuid(&impl_->event_context);
  impl_->callback.Attach(new VolumeCallback(impl_.get()));
  hr = impl_->volume->RegisterControlChangeNotify(impl_->callback.Get());
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] RegisterControlChangeNotify failed: 0x%08lx\n",
            (unsigned long)hr);
    impl_->callback.Reset();
    return false;
  }

  return true;
}

void WinVolumeController::shutdown() {
  if (!impl_)
    return;
  ScopedComApartment apartment;

  if (impl_->volume && impl_->callback) {
    impl_->volume->UnregisterControlChangeNotify(impl_->callback.Get());
  }
  impl_->callback.Reset();
  impl_->volume.Reset();
  impl_->endpoint.Reset();
  impl_->enumerator.Reset();
}

void WinVolumeController::set_state_callback(StateCallback cb) {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state_callback = std::move(cb);
}

void WinVolumeController::push_current_state() {
  if (!impl_ || !impl_->volume)
    return;
  ScopedComApartment apartment;

  float scalar = 0.0f;
  BOOL muted = FALSE;
  if (FAILED(impl_->volume->GetMasterVolumeLevelScalar(&scalar)) ||
      FAILED(impl_->volume->GetMute(&muted))) {
    return;
  }

  StateCallback callback;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    callback = impl_->state_callback;
  }
  if (callback) {
    callback(static_cast<uint16_t>(scalar * 65535.0f + 0.5f), muted != FALSE);
  }
}

bool WinVolumeController::set_volume(uint16_t level, bool muted) {
  if (!impl_ || !impl_->volume)
    return false;
  ScopedComApartment apartment;

  float scalar = static_cast<float>(level) / 65535.0f;
  HRESULT hr =
      impl_->volume->SetMasterVolumeLevelScalar(scalar, &impl_->event_context);
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] SetMasterVolumeLevelScalar failed: 0x%08lx\n",
            (unsigned long)hr);
    return false;
  }

  hr = impl_->volume->SetMute(muted ? TRUE : FALSE, &impl_->event_context);
  if (FAILED(hr)) {
    fprintf(stderr, "[volume] SetMute failed: 0x%08lx\n", (unsigned long)hr);
    return false;
  }

  push_current_state();
  return true;
}

} // namespace droidscreen

#include "uwp_output.h"

#include <array>
#include <atomic>
#include <windows.h>
#include <xaudio2.h>

namespace engine::audio::detail {

struct UwpOutput::Impl final : IXAudio2VoiceCallback
{
    static constexpr core::u32 Frames = 480;
    static constexpr core::u32 Channels = 2;
    struct Buffer
    {
        std::array<float, Frames * Channels> samples{};
    };
    std::array<Buffer, 3> buffers;
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* mastering = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    Render render = nullptr;
    void* context = nullptr;
    std::atomic<bool> stopping{false};

    ~Impl()
    {
        stopping.store(true, std::memory_order_release);
        // DestroyVoice waits for outstanding callbacks; their buffers and mixer
        // therefore outlive every access from the audio processing thread.
        if (source != nullptr)
            source->DestroyVoice();
        if (mastering != nullptr)
            mastering->DestroyVoice();
        if (engine != nullptr)
            engine->Release();
    }

    bool submit(Buffer& buffer)
    {
        render(context, buffer.samples.data(), Frames);
        XAUDIO2_BUFFER descriptor{};
        descriptor.AudioBytes = static_cast<UINT32>(sizeof(buffer.samples));
        descriptor.pAudioData = reinterpret_cast<const BYTE*>(buffer.samples.data());
        descriptor.pContext = &buffer;
        return SUCCEEDED(source->SubmitSourceBuffer(&descriptor));
    }

    void STDMETHODCALLTYPE OnBufferEnd(void* value) override
    {
        if (!stopping.load(std::memory_order_acquire) && !submit(*static_cast<Buffer*>(value)))
            stopping.store(true, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override { stopping.store(true, std::memory_order_release); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
};

UwpOutput::UwpOutput() = default;
UwpOutput::~UwpOutput() = default;

bool UwpOutput::start(Render render, void* context)
{
    auto output = std::make_unique<Impl>();
    output->render = render;
    output->context = context;
    if (FAILED(XAudio2Create(&output->engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(output->engine->CreateMasteringVoice(&output->mastering, Impl::Channels, 48000, 0, nullptr, nullptr,
                                                    AudioCategory_GameEffects)))
        return false;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = Impl::Channels;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = Impl::Channels * sizeof(float);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (FAILED(output->engine->CreateSourceVoice(&output->source, &format, XAUDIO2_VOICE_NOSRC, 1.0f, output.get())))
        return false;
    for (auto& buffer : output->buffers) {
        if (!output->submit(buffer))
            return false;
    }
    if (FAILED(output->source->Start()))
        return false;
    m_impl = std::move(output);
    return true;
}

} // namespace engine::audio::detail

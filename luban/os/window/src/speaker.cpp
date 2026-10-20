// A system's sound on this computer (host.cppm; design 2026-10-20 §20.1):
// SDL's audio -- whatever this computer plays through -- fed with the
// samples an openhal audio stream carries.
module;

#include <SDL3/SDL.h>

module luban.desktop.host;

import std;

namespace luban::desktop::host {

struct Speaker::Impl {
    SDL_AudioStream* stream { nullptr };
    std::size_t limit { 0 };            // bytes: half a second
    std::size_t frame { 4 };
};

bool Speaker::prepare() {
    return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
}

std::expected<std::unique_ptr<Speaker>, std::string> Speaker::open(std::uint32_t rate, std::uint32_t channels) {
    if (rate == 0 || channels == 0) return std::unexpected(std::string("no format to play"));
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) return std::unexpected(std::string("no sound here (SDL's audio was not started)"));
    const SDL_AudioSpec spec{.format = SDL_AUDIO_S16LE, .channels = static_cast<int>(channels), .freq = static_cast<int>(rate)};
    auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) return std::unexpected(std::format("no sound output here: {}", SDL_GetError()));
    std::unique_ptr<Speaker> s(new Speaker());
    s->impl_ = std::make_unique<Impl>();
    s->impl_->stream = stream;
    s->impl_->frame = std::size_t{channels} * 2;
    s->impl_->limit = std::size_t{rate} * s->impl_->frame / 2;
    SDL_ResumeAudioStreamDevice(stream);
    return s;
}

Speaker::~Speaker() {
    if (impl_ && impl_->stream) SDL_DestroyAudioStream(impl_->stream);
}

void Speaker::play(std::string_view samples) {
    if (!impl_ || !impl_->stream || samples.empty()) return;
    // Behind by more than the limit: what is queued goes, this plays now.
    if (SDL_GetAudioStreamQueued(impl_->stream) > static_cast<int>(impl_->limit)) SDL_ClearAudioStream(impl_->stream);
    const auto whole = samples.size() - samples.size() % impl_->frame;
    SDL_PutAudioStreamData(impl_->stream, samples.data(), static_cast<int>(whole));
}

void Speaker::volume(double level, bool muted) {
    if (!impl_ || !impl_->stream) return;
    SDL_SetAudioStreamGain(impl_->stream, muted ? 0.0f : static_cast<float>(std::clamp(level, 0.0, 1.0)));
}

std::size_t Speaker::queued() const {
    if (!impl_ || !impl_->stream) return 0;
    return static_cast<std::size_t>(std::max(0, SDL_GetAudioStreamQueued(impl_->stream)));
}

}  // namespace luban::desktop::host

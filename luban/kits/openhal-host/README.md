# openhal-host

openhal's device source for the machine a program runs on.

```cpp
import openhal;
import openhal.host;

const openhal::host::Host here;
static_assert(openhal::Source<openhal::host::Host>);
auto gpus = here.list(openhal::Kind::Gpu);
```

| Platform | GPU | Display | Audio | Input |
|---|---|---|---|---|
| Linux | DRM (`/sys/class/drm`), NVIDIA's `/proc/driver/nvidia`, WSL's `/dev/dxg` | connected DRM connectors, EDID | ALSA (`/proc/asound`) | evdev (`/sys/class/input`) |
| macOS | IOKit accelerators | CoreGraphics | CoreAudio | could not read: not enumerated here yet |
| Windows | DXGI adapters | the display device list | waveOut / waveIn | raw input |

What cannot be read is said with its reason, never as an empty list.


# OBS-studio WebRTC

This project is a fork of OBS-studio with support for WebRTC. WebRTC supports comes from the inclusion of the open source implementation from webrtc.org used (at least in part) by chrome, firefox, and safari.

The implementation is in the "plugins / obs-outputs" directory, co-existing with the flash and FTL output plugins. The WebRTCStream files contain the high-level implementation, while the xxxx-stream files contain the specific implementation for a given service.

For the time being the following services and sites are supported:
- Janus "videoRoom" server (Using WHIP)
- Millicast.com PaaS

Do not forget to share the love with the original OBS-Studio project and its fantastic team [there](https://obsproject.com/blog/progress-report-february-2019).

# Janus support

No support for Janus will be provided by default. Bruden of proof (that there is a bug on the OS Side) is on the one(s) opening the tickets.
There is no commercial support for this fork of OBS.
Janus support will only be done through the WHIP protocol.
Commercial uspport for Janus server side is available [here](https://janus.conf.meetecho.com/support).

# Binaries

Pre-built and tested Binaries are available [here](https://github.com/CoSMoSoftware/OBS-studio-webrtc/releases).

# Compilation, Installation and Packaging

Follow the original compilation, Installation and packaging guide https://github.com/obsproject/obs-studio

In addition, please read https://github.com/CoSMoSoftware/OBS-studio-webrtc/wiki/Dev-process to adapt the build process when required.

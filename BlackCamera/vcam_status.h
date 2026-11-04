#pragma once
// Shared state names for the virtual camera outputs.

// Which output the user asked for (persisted in settings.ini as vcam_backend)
enum class VcamBackend : int {
    Auto    = 0, // built-in Windows 11 camera, OBS Virtual Camera when that is not available
    Windows = 1, // built-in camera only (MFCreateVirtualCamera, Windows 11)
    Obs     = 2, // OBS Virtual Camera only
};

enum class VcamStatus : int {
    Off,
    NotInstalled, // OBS: no OBS Studio; Windows: BlackCameraVCam.dll is not registered
    Unsupported,  // Windows: needs Windows 11 (build 22000+)
    Busy,         // OBS: another producer (usually OBS itself) owns the shared memory
    Error,
    Waiting,      // Windows: camera registered, no app is reading it yet
    Streaming,
};

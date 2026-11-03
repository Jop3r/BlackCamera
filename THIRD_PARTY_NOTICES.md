# Third-party components

BlackCamera is distributed together with the following components. Their
licenses apply to those components only; BlackCamera itself is MIT-licensed
(see `LICENSE`).

| Component | License | Source |
|-----------|---------|--------|
| OpenCV 4.x (and the libraries its DLLs bundle: protobuf, abseil, libjpeg-turbo, libpng, libtiff, libwebp, zlib, liblzma) | Apache-2.0 (OpenCV); BSD / zlib / IJG / libtiff / public-domain style licenses for the bundled libraries | https://github.com/opencv/opencv |
| YuNet face detection model (`face_detection_yunet_2023mar.onnx`) | MIT | https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet |
| GLFW 3 | zlib/libpng | https://www.glfw.org |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| Microsoft Visual C++ Redistributable (installer only) | Microsoft Software License Terms | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |

The full license texts of the vcpkg-built libraries are in
`vcpkg_installed/x64-windows/share/<package>/copyright` of a CMake build.

OBS Virtual Camera support talks to the virtual camera driver that ships with
OBS Studio through its shared-memory interface; no OBS code is included.

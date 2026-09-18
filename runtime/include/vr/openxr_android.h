// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)

#include "vr/openxr_runtime.h"

#include <string>

namespace mkw::vr {

// Android-specific OpenXR bootstrap.
//
// The Khronos loader on Android must be told which JavaVM and Context it lives
// in before any other OpenXR entry point is called (xrInitializeLoaderKHR), and
// xrCreateInstance must chain an XrInstanceCreateInfoAndroidKHR naming the
// activity. Both come from SDL's Android glue (the SDLActivity that hosts the
// runtime), so no extra JNI surface is needed. DolphinXR found that the loader
// context must be the *activity*, not the application context: with a plain
// application context the Quest runtime never leaves XR_SESSION_STATE_IDLE.
//
// Call order: OpenXRAndroidInitializeLoader() once, before OpenXRRuntime::Initialize;
// then pass OpenXRAndroidInstanceCreateNext() as OpenXRConfig::instance_create_next.
bool OpenXRAndroidInitializeLoader(OpenXRLogCallback logger, std::string* error);
const void* OpenXRAndroidInstanceCreateNext();

// Optional XR_KHR_android_thread_settings hint for the calling thread. Failure
// is not an error: some Quest runtime builds advertise the extension but reject
// particular thread types, so the caller only logs the outcome.
enum class OpenXRAndroidThreadType {
    ApplicationMain,
    ApplicationWorker,
    RendererMain,
    RendererWorker,
};
bool OpenXRAndroidRegisterThread(OpenXRRuntime& runtime, OpenXRAndroidThreadType type);
// The same hint for another thread, named by its Linux thread id (gettid).
bool OpenXRAndroidRegisterThreadId(OpenXRRuntime& runtime, OpenXRAndroidThreadType type, uint32_t thread_id);

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(__ANDROID__)

#pragma once

// Android-only diagnostic switches read from system properties, so an on-device
// experiment can change renderer behaviour with `adb shell setprop` and an app
// restart instead of a rebuild. debug.* properties are writable by the adb shell
// and readable by apps. Values are read once per process by each caller.

#if defined(__ANDROID__)
#include <sys/system_properties.h>

#include <cstdlib>

namespace aurora::android_debug {

inline int property_int(const char* name, int fallback) noexcept {
  char value[PROP_VALUE_MAX]{};
  if (__system_property_get(name, value) <= 0) {
    return fallback;
  }
  return std::atoi(value);
}

} // namespace aurora::android_debug
#endif

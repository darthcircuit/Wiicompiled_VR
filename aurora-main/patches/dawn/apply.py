"""Apply the versioned Aurora native Vulkan ABI to the pinned Dawn source tree."""
from pathlib import Path
import shutil, sys
root = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
for name in ("aurora_vulkan_hooks.h", "aurora_vulkan_interop.inc"):
    shutil.copyfile(here / name, root / "src/dawn/native/vulkan" / name)
shutil.copyfile(here.parent.parent / "include/aurora/dawn_vulkan_abi.h",
                root / "src/dawn/native/vulkan/aurora_dawn_vulkan_abi.h")
p = root / "src/dawn/native/vulkan/VulkanBackend.cpp"
s = p.read_text()
if '#include "aurora_vulkan_interop.inc"' not in s:
    s += '\n#include "aurora_vulkan_interop.inc"\n'
p.write_text(s)
p = root / "src/dawn/common/DynamicLib.cpp"
s = p.read_text()
# LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires an absolute filename. Vulkan's
# system loader is also tried by bare name; preserve the restricted default
# search directories for that case instead of failing with ERROR_INVALID_PARAMETER.
old = 'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;'
new = '''((filename.size() > 2 && filename[1] == ':') || filename.starts_with("\\\\\\\\")
             ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR : 0) | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;'''
if old in s:
    s = s.replace(old, new, 1)
p.write_text(s)
p = root / "src/dawn/native/vulkan/VulkanFunctions.cpp"
s = p.read_text()
if '#include "aurora_vulkan_hooks.h"' not in s:
    marker = 'namespace dawn::native::vulkan {'
    assert marker in s
    s = s.replace(marker, '#include "aurora_vulkan_hooks.h"\n\n' + marker, 1)
    for old, new in [
        ('GET_GLOBAL_PROC(CreateInstance);', 'CreateInstance = AuroraCreateInstance(GetInstanceProcAddr);'),
        ('GET_INSTANCE_PROC(CreateDevice);', 'CreateDevice = AuroraCreateDevice(GetInstanceProcAddr, instance);'),
        ('GET_INSTANCE_PROC(EnumeratePhysicalDevices);', 'EnumeratePhysicalDevices = AuroraEnumeratePhysicalDevices(GetInstanceProcAddr, instance);')]:
        assert old in s, old
        s = s.replace(old, old + '\n    ' + new, 1)
p.write_text(s)

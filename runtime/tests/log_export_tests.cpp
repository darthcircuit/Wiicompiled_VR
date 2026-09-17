// SPDX-License-Identifier: GPL-3.0-or-later
// F10 > Diagnostics > Export Logs against a synthetic application data folder.

#include "log_export.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

void RequireAt(bool condition, int line, const char* expression) {
    if (!condition) {
        std::cerr << "log_export_tests.cpp:" << line << ": requirement failed: " << expression << '\n';
        std::abort();
    }
}
#define Require(condition) RequireAt((condition), __LINE__, #condition)

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

size_t CountExports(const fs::path& parent) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(parent)) {
        count += entry.path().filename().string().rfind(log_export::kExportFolderPrefix, 0) == 0 ? 1 : 0;
    }
    return count;
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "wiicompiled_log_export_tests";
    fs::remove_all(root);
    const fs::path data = root / "WiiCompiledOpenXRVR";
    const fs::path logs = data / "Logs";
    const fs::path config = data / "Config.toml";
    const fs::path target = root / "Chosen folder";
    fs::create_directories(target);

    Write(logs / "base_100_pid1" / "console.log", "[runtime] first run\n");
    Write(logs / "base_200_pid2" / "console.log", "[runtime] [xr-diag] 1.00s 72.0Hz cycles=72\n");
    Write(logs / "base_200_pid2" / "crash.txt", "");
    Write(config, "[vr]\nenabled = true\n");

    // The running session's transcript is still open for writing while it is exported.
    fs::create_directories(logs / "base_300_pid3");
    std::ofstream live(logs / "base_300_pid3" / "console.log", std::ios::binary);
    Require(static_cast<bool>(live));
    live << "[runtime] live session\n";
    live.flush();

    const auto now = std::chrono::system_clock::now();
    const auto result = log_export::ExportLogs(logs, config, target, "Exported by process 3.\n", now);
    Require(result.Succeeded());
    Require(result.files_copied == 5);
    Require(result.files_failed == 0);
    Require(result.destination.parent_path() == target);
    Require(result.destination.filename().string().rfind(log_export::kExportFolderPrefix, 0) == 0);
    Require(Read(result.destination / "Logs" / "base_200_pid2" / "console.log") ==
            "[runtime] [xr-diag] 1.00s 72.0Hz cycles=72\n");
    Require(fs::exists(result.destination / "Logs" / "base_200_pid2" / "crash.txt"));
    Require(fs::file_size(result.destination / "Logs" / "base_200_pid2" / "crash.txt") == 0);
    Require(Read(result.destination / "Logs" / "base_300_pid3" / "console.log") == "[runtime] live session\n");
    Require(Read(result.destination / "Config.toml") == "[vr]\nenabled = true\n");
    const std::string info = Read(result.destination / "export-info.txt");
    Require(info.rfind("Exported ", 0) == 0);
    Require(info.find("Exported by process 3.\n") != std::string::npos);
    live.close();

    // Exporting twice in the same second makes a second folder, not a merge.
    const auto again = log_export::ExportLogs(logs, config, target, {}, now);
    Require(again.Succeeded());
    Require(again.destination != result.destination);
    Require(CountExports(target) == 2);

    // Choosing the Logs folder itself must not copy the export into itself.
    const auto inside = log_export::ExportLogs(logs, config, logs, {}, now);
    Require(inside.Succeeded());
    Require(inside.files_copied == 5);
    Require(!fs::exists(inside.destination / "Logs" / inside.destination.filename()));

    // Missing sources: only the configuration exists.
    const auto config_only = log_export::ExportLogs(root / "missing Logs", config, target, {}, now);
    Require(config_only.Succeeded());
    Require(config_only.files_copied == 1);

    const auto nothing = log_export::ExportLogs(root / "missing Logs", root / "missing.toml", target, {}, now);
    Require(!nothing.Succeeded());
    Require(!nothing.destination.empty());
    Require(!nothing.error.empty());

    const auto bad_parent = log_export::ExportLogs(logs, config, root / "does not exist", {}, now);
    Require(!bad_parent.Succeeded());
    Require(bad_parent.destination.empty());

    fs::remove_all(root);
    std::cout << "Log export tests passed\n";
}

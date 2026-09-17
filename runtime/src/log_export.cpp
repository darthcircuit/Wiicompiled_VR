// SPDX-License-Identifier: GPL-3.0-or-later

#include "log_export.h"

#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

namespace log_export {
namespace {

// UTF-8, like every narrow path string in the runtime.
std::string ExportPathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::tm ExportLocalTime(std::chrono::system_clock::time_point now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local;
}

std::string FormatExportTime(std::chrono::system_clock::time_point now, const char* format) {
    const std::tm local = ExportLocalTime(now);
    char buffer[64];
    const size_t length = std::strftime(buffer, sizeof(buffer), format, &local);
    return std::string(buffer, length);
}

bool CopyExportFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                    std::string& error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "could not read " + ExportPathText(source);
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not write " + ExportPathText(destination);
        return false;
    }
    // A loop rather than `output << input.rdbuf()`, which flags an empty file
    // as a failed insertion.
    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
        }
    }
    if (input.bad() || !output) {
        error = "could not copy " + ExportPathText(source);
        return false;
    }
    return true;
}

} // namespace

Result ExportLogs(const std::filesystem::path& logs_directory, const std::filesystem::path& config_file,
                  const std::filesystem::path& parent, std::string_view note,
                  std::chrono::system_clock::time_point now) {
    Result result;
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        result.error = "the chosen folder does not exist: " + ExportPathText(parent);
        return result;
    }

    const std::string base = std::string(kExportFolderPrefix) + FormatExportTime(now, "%Y%m%d-%H%M%S");
    std::filesystem::path destination = parent / base;
    for (int suffix = 2; std::filesystem::exists(destination, ec); ++suffix) {
        if (suffix > 99) {
            result.error = "too many exports named " + base + " in " + ExportPathText(parent);
            return result;
        }
        destination = parent / (base + "-" + std::to_string(suffix));
    }
    if (!std::filesystem::create_directories(destination, ec)) {
        result.error = "could not create " + ExportPathText(destination) + (ec ? ": " + ec.message() : "");
        return result;
    }
    result.destination = destination;

    const auto copy = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::string error;
        if (CopyExportFile(from, to, error)) {
            ++result.files_copied;
        } else {
            ++result.files_failed;
            if (result.error.empty()) {
                result.error = std::move(error);
            }
        }
    };

    if (std::filesystem::is_directory(logs_directory, ec)) {
        const std::filesystem::path logs_target = destination / "Logs";
        std::filesystem::create_directories(logs_target, ec);
        // Choosing the Logs folder itself as the destination must not copy the
        // export into itself.
        const std::filesystem::path own_folder = std::filesystem::weakly_canonical(destination, ec);
        std::filesystem::recursive_directory_iterator entry(
            logs_directory, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && entry != std::filesystem::recursive_directory_iterator(); entry.increment(ec)) {
            const std::filesystem::path target = logs_target / entry->path().lexically_relative(logs_directory);
            std::error_code entry_ec;
            if (entry->is_directory(entry_ec)) {
                if (std::filesystem::weakly_canonical(entry->path(), entry_ec) == own_folder) {
                    entry.disable_recursion_pending();
                    continue;
                }
                std::filesystem::create_directories(target, entry_ec);
                continue;
            }
            if (!entry->is_regular_file(entry_ec)) {
                continue;
            }
            std::filesystem::create_directories(target.parent_path(), entry_ec);
            copy(entry->path(), target);
        }
        if (ec) {
            ++result.files_failed;
            if (result.error.empty()) {
                result.error = "could not list " + ExportPathText(logs_directory) + ": " + ec.message();
            }
        }
    }
    if (std::filesystem::is_regular_file(config_file, ec)) {
        copy(config_file, destination / config_file.filename());
    }

    std::ofstream info(destination / "export-info.txt", std::ios::binary | std::ios::trunc);
    info << "Exported " << FormatExportTime(now, "%Y-%m-%d %H:%M:%S") << " (local time)\n" << note;
    if (!note.empty() && note.back() != '\n') {
        info << '\n';
    }

    if (result.files_copied == 0 && result.files_failed == 0) {
        result.error = "no logs or configuration were found to export";
    }
    return result;
}

} // namespace log_export

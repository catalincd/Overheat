#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace oh {

class App;

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    fs::path path;
};

// C API for plugins (loaded via dlopen)
extern "C" {
    struct OHPluginAPI {
        void* app_handle;
        // Editor operations
        void (*insert_text)(void* handle, const char* text);
        const char* (*get_selection)(void* handle);
        void (*open_file)(void* handle, const char* path);
        void (*save_file)(void* handle);
        // Commands
        void (*register_command)(void* handle, const char* name, const char* label, void (*fn)(void*), void* user_data);
        // Status
        void (*set_status_message)(void* handle, const char* msg);
        // Info
        const char* (*current_file_path)(void* handle);
        int (*cursor_line)(void* handle);
        int (*cursor_col)(void* handle);
    };

    typedef int (*PluginInitFn)(OHPluginAPI* api);
    typedef void (*PluginShutdownFn)(void);
    typedef const char* (*PluginNameFn)(void);
    typedef const char* (*PluginVersionFn)(void);
}

class PluginManager {
public:
    void init(App* app);
    void shutdown();

    bool load_plugin(const fs::path& path);
    void unload_plugin(const std::string& id);

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    void scan_directory(const fs::path& dir);

private:
    App* app_ = nullptr;
    OHPluginAPI api_;
    std::vector<PluginInfo> plugins_;
    std::vector<void*> handles_; // dlopen handles
};

} // namespace oh

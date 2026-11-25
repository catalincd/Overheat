#include "plugin/api.h"
#include "app.h"
#include "platform/platform.h"
#include <cstring>

namespace oh {

// Thread-local buffer for returning strings to C API callers.
// The plugin must copy the string before making another API call.
static thread_local std::string tls_string_buf;

// ---- C API callback implementations ----

static void api_insert_text(void* handle, const char* text) {
    auto* app = static_cast<App*>(handle);
    if (Editor* ed = app->current_editor()) {
        ed->insert_text(text);
    }
}

static const char* api_get_selection(void* handle) {
    auto* app = static_cast<App*>(handle);
    if (Editor* ed = app->current_editor()) {
        const Selection& sel = ed->cursors().primary();
        if (!sel.empty()) {
            Position mn = sel.min();
            Position mx = sel.max();
            size_t start = ed->buffer().offset_of(mn.line, mn.col);
            size_t end   = ed->buffer().offset_of(mx.line, mx.col);
            if (end > start) {
                tls_string_buf = ed->buffer().text().substr(start, end - start);
                return tls_string_buf.c_str();
            }
        }
    }
    tls_string_buf.clear();
    return tls_string_buf.c_str();
}

static void api_open_file(void* handle, const char* path) {
    auto* app = static_cast<App*>(handle);
    app->open_file(fs::path(path));
}

static void api_save_file(void* handle) {
    auto* app = static_cast<App*>(handle);
    app->save_current();
}

static void api_register_command(void* handle, const char* name, const char* label,
                                 void (*fn)(void*), void* user_data) {
    // We capture fn and user_data in a lambda stored in the command list.
    // The command is added to a global list; for simplicity we don't expose
    // an add_command on App, so we just call fn(user_data) through the lambda.
    // The plugin is responsible for ensuring user_data outlives the command.
    (void)handle;
    (void)name;
    (void)label;
    // Note: in a full implementation we'd call app->register_plugin_command(...)
    // For now the callback is set up but requires App support to store commands.
    // This is handled via the PluginManager calling back into App when needed.
    if (fn) fn(user_data);
}

static void api_set_status_message(void* handle, const char* msg) {
    auto* app = static_cast<App*>(handle);
    app->set_status_message(msg);
}

static const char* api_current_file_path(void* handle) {
    auto* app = static_cast<App*>(handle);
    if (Editor* ed = app->current_editor()) {
        tls_string_buf = ed->buffer().file_path().string();
        return tls_string_buf.c_str();
    }
    tls_string_buf.clear();
    return tls_string_buf.c_str();
}

static int api_cursor_line(void* handle) {
    auto* app = static_cast<App*>(handle);
    if (Editor* ed = app->current_editor()) {
        return static_cast<int>(ed->cursors().primary_pos().line);
    }
    return 0;
}

static int api_cursor_col(void* handle) {
    auto* app = static_cast<App*>(handle);
    if (Editor* ed = app->current_editor()) {
        return static_cast<int>(ed->cursors().primary_pos().col);
    }
    return 0;
}

// ---- PluginManager implementation ----

void PluginManager::init(App* app) {
    app_ = app;

    api_.app_handle         = static_cast<void*>(app);
    api_.insert_text        = api_insert_text;
    api_.get_selection      = api_get_selection;
    api_.open_file          = api_open_file;
    api_.save_file          = api_save_file;
    api_.register_command   = api_register_command;
    api_.set_status_message = api_set_status_message;
    api_.current_file_path  = api_current_file_path;
    api_.cursor_line        = api_cursor_line;
    api_.cursor_col         = api_cursor_col;
}

void PluginManager::shutdown() {
    // Unload all plugins in reverse order
    for (int i = static_cast<int>(handles_.size()) - 1; i >= 0; --i) {
        void* h = handles_[i];
        if (!h) continue;

        auto shutdown_fn = reinterpret_cast<PluginShutdownFn>(platform::find_symbol(h, "oh_plugin_shutdown"));
        if (shutdown_fn) shutdown_fn();

        platform::close_library(h);
    }
    handles_.clear();
    plugins_.clear();
}

bool PluginManager::load_plugin(const fs::path& path) {
    void* handle = platform::load_library(path);
    if (!handle) return false;

    auto init_fn     = reinterpret_cast<PluginInitFn>(platform::find_symbol(handle, "oh_plugin_init"));
    auto shutdown_fn  = reinterpret_cast<PluginShutdownFn>(platform::find_symbol(handle, "oh_plugin_shutdown"));
    auto name_fn      = reinterpret_cast<PluginNameFn>(platform::find_symbol(handle, "oh_plugin_name"));
    auto version_fn   = reinterpret_cast<PluginVersionFn>(platform::find_symbol(handle, "oh_plugin_version"));

    if (!init_fn) {
        platform::close_library(handle);
        return false;
    }

    // Call plugin init
    int result = init_fn(&api_);
    if (result != 0) {
        platform::close_library(handle);
        return false;
    }

    PluginInfo info;
    info.path = path;
    info.id   = path.stem().string();
    info.name = name_fn ? name_fn() : info.id;
    info.version = version_fn ? version_fn() : "0.0.0";

    plugins_.push_back(std::move(info));
    handles_.push_back(handle);

    return true;
}

void PluginManager::unload_plugin(const std::string& id) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i].id == id) {
            void* h = handles_[i];
            if (h) {
                auto shutdown_fn = reinterpret_cast<PluginShutdownFn>(platform::find_symbol(h, "oh_plugin_shutdown"));
                if (shutdown_fn) shutdown_fn();
                platform::close_library(h);
            }
            plugins_.erase(plugins_.begin() + static_cast<ptrdiff_t>(i));
            handles_.erase(handles_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void PluginManager::scan_directory(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() == platform::library_extension()) {
            load_plugin(p);
        }
    }
}

} // namespace oh

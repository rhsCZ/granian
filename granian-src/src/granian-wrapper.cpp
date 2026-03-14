#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

volatile sig_atomic_t g_terminate_requested = 0;

struct Config {
    std::string app;
    std::string working_directory;
    std::string venv;
    std::vector<std::string> passthrough_args;
    std::vector<std::pair<std::string, std::string>> env_vars;
    std::map<std::string, std::vector<std::string>> options;
};

struct AppInstance {
    std::string name;
    std::string config_path;
    std::string config_dir;
    Config config;
    pid_t pid = -1;
};

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool has_suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool parse_bool(const std::string& raw, bool& out) {
    const std::string value = trim(raw);
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

std::string unquote(const std::string& raw) {
    const std::string value = trim(raw);
    if (value.size() < 2) {
        return value;
    }
    const char quote = value.front();
    if ((quote != '"' && quote != '\'') || value.back() != quote) {
        return value;
    }

    std::string result;
    result.reserve(value.size() - 2);
    bool escaped = false;
    for (std::size_t index = 1; index + 1 < value.size(); ++index) {
        const char ch = value[index];
        if (escaped) {
            result.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        result.push_back(ch);
    }
    if (escaped) {
        result.push_back('\\');
    }
    return result;
}

std::string normalize_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(key.begin(), key.end(), '_', '-');
    return key;
}

std::string base_name_without_suffix(const std::string& path) {
    const auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (has_suffix(name, ".conf")) {
        name.resize(name.size() - 5);
    }
    return name;
}

void add_config_value(Config& config, const std::string& source, int line_number,
                      const std::string& raw_key, const std::string& raw_value) {
    const std::string key = normalize_key(trim(raw_key));
    const std::string value = unquote(raw_value);
    if (key.empty()) {
        throw std::runtime_error(source + ":" + std::to_string(line_number) + ": empty key");
    }
    if (key == "app") {
        config.app = value;
        return;
    }
    if (key == "working-directory") {
        config.working_directory = value;
        return;
    }
    if (key == "venv") {
        config.venv = value;
        return;
    }
    if (key == "arg") {
        if (!value.empty()) {
            config.passthrough_args.push_back(value);
        }
        return;
    }
    if (key == "env") {
        const auto separator = value.find('=');
        if (separator == std::string::npos || separator == 0) {
            throw std::runtime_error(source + ":" + std::to_string(line_number) +
                                     ": env must use NAME=VALUE");
        }
        config.env_vars.emplace_back(value.substr(0, separator), value.substr(separator + 1));
        return;
    }
    config.options[key].push_back(value);
}

void parse_file(Config& config, const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }

        const auto separator = stripped.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(path + ":" + std::to_string(line_number) +
                                     ": expected key=value");
        }
        add_config_value(config, path, line_number,
                         stripped.substr(0, separator), stripped.substr(separator + 1));
    }
}

std::vector<std::string> list_config_dir(const std::string& path) {
    std::vector<std::string> files;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        if (errno == ENOENT) {
            return files;
        }
        throw std::runtime_error("cannot open config dir: " + path + ": " + std::strerror(errno));
    }

    while (const dirent* entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name == "." || name == ".." || !has_suffix(name, ".conf")) {
            continue;
        }
        files.push_back(path + "/" + name);
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
}

void load_optional_config(Config& config, const std::string& config_path, const std::string& config_dir) {
    struct stat statbuf {};
    if (!config_path.empty() && stat(config_path.c_str(), &statbuf) == 0) {
        parse_file(config, config_path);
    }
    if (!config_dir.empty()) {
        for (const auto& file : list_config_dir(config_dir)) {
            parse_file(config, file);
        }
    }
}

std::vector<std::string> build_exec_args(const Config& config) {
    if (config.app.empty()) {
        throw std::runtime_error("missing required 'app=' setting");
    }

    std::vector<std::string> final_args;
    final_args.emplace_back(GRANIAN_BIN_PATH);

    for (const auto& [key, values] : config.options) {
        for (const auto& value : values) {
            bool bool_value = false;
            if (parse_bool(value, bool_value)) {
                final_args.emplace_back(bool_value ? "--" + key : "--no-" + key);
                continue;
            }
            final_args.emplace_back("--" + key);
            if (!value.empty()) {
                final_args.push_back(value);
            }
        }
    }

    final_args.insert(final_args.end(), config.passthrough_args.begin(), config.passthrough_args.end());
    final_args.push_back(config.app);
    return final_args;
}

bool is_directory(const std::string& path) {
    struct stat statbuf {};
    return stat(path.c_str(), &statbuf) == 0 && S_ISDIR(statbuf.st_mode);
}

bool is_executable_file(const std::string& path) {
    struct stat statbuf {};
    return stat(path.c_str(), &statbuf) == 0 && S_ISREG(statbuf.st_mode) &&
           access(path.c_str(), X_OK) == 0;
}

std::string join_path(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

std::string configured_granian_path(const Config& config) {
    if (config.venv.empty()) {
        return GRANIAN_BIN_PATH;
    }

    if (!is_directory(config.venv)) {
        throw std::runtime_error("venv directory does not exist: " + config.venv);
    }

    const std::string candidate = join_path(join_path(config.venv, "bin"), "granian");
    if (is_executable_file(candidate)) {
        return candidate;
    }
    return GRANIAN_BIN_PATH;
}

void apply_venv_env(const Config& config) {
    if (config.venv.empty()) {
        return;
    }

    const std::string bin_dir = join_path(config.venv, "bin");
    const char* current_path = std::getenv("PATH");
    const std::string new_path = current_path && *current_path
        ? bin_dir + ":" + current_path
        : bin_dir;

    if (setenv("VIRTUAL_ENV", config.venv.c_str(), 1) != 0) {
        throw std::runtime_error("setenv(VIRTUAL_ENV) failed: " + std::string(std::strerror(errno)));
    }
    if (setenv("PATH", new_path.c_str(), 1) != 0) {
        throw std::runtime_error("setenv(PATH) failed: " + std::string(std::strerror(errno)));
    }
}

void apply_env(const Config& config) {
    for (const auto& [name, value] : config.env_vars) {
        if (setenv(name.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("setenv(" + name + ") failed: " + std::strerror(errno));
        }
    }
}

void reset_signal_handlers() {
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGHUP, SIG_DFL);
    std::signal(SIGCHLD, SIG_DFL);
}

pid_t start_child(const AppInstance& instance) {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed for " + instance.name + ": " + std::strerror(errno));
    }
    if (pid == 0) {
        reset_signal_handlers();
        if (!instance.config.working_directory.empty() &&
            chdir(instance.config.working_directory.c_str()) != 0) {
            std::cerr << "granian-wrapper: chdir(" << instance.config.working_directory
                      << ") failed for " << instance.name << ": " << std::strerror(errno) << "\n";
            _exit(1);
        }
        if (setenv("GRANIAN_INSTANCE", instance.name.c_str(), 1) != 0) {
            std::cerr << "granian-wrapper: setenv(GRANIAN_INSTANCE) failed for " << instance.name
                      << ": " << std::strerror(errno) << "\n";
            _exit(1);
        }
        try {
            apply_venv_env(instance.config);
            apply_env(instance.config);
            std::vector<std::string> final_args = build_exec_args(instance.config);
            final_args[0] = configured_granian_path(instance.config);
            std::vector<char*> exec_argv;
            exec_argv.reserve(final_args.size() + 1);
            for (auto& arg : final_args) {
                exec_argv.push_back(arg.data());
            }
            exec_argv.push_back(nullptr);
            execv(final_args[0].c_str(), exec_argv.data());
            std::cerr << "granian-wrapper: execv failed for " << instance.name << ": "
                      << std::strerror(errno) << "\n";
        } catch (const std::exception& error) {
            std::cerr << "granian-wrapper: " << error.what() << "\n";
        }
        _exit(1);
    }
    return pid;
}

void signal_handler(int signo) {
    if (signo == SIGTERM || signo == SIGINT) {
        g_terminate_requested = 1;
    }
}

void install_signal_handlers() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGHUP, SIG_IGN);
}

std::vector<std::string> list_enabled_apps() {
    return list_config_dir(GRANIAN_APPS_ENABLED_DIR);
}

std::vector<AppInstance> load_apps() {
    std::vector<AppInstance> apps;
    Config global_defaults;
    load_optional_config(global_defaults, GRANIAN_CONFIG_PATH, GRANIAN_CONFIG_DIR);

    for (const auto& path : list_enabled_apps()) {
        AppInstance instance;
        instance.name = base_name_without_suffix(path);
        instance.config_path = path;
        instance.config_dir = std::string(GRANIAN_APPS_ENABLED_DIR) + "/" + instance.name + ".d";
        instance.config = global_defaults;
        load_optional_config(instance.config, instance.config_path, instance.config_dir);
        if (instance.config.app.empty()) {
            throw std::runtime_error("missing required 'app=' setting in " + instance.config_path);
        }
        apps.push_back(instance);
    }

    return apps;
}

void validate_apps() {
    const auto apps = load_apps();
    if (apps.empty()) {
        throw std::runtime_error("no enabled apps found in " + std::string(GRANIAN_APPS_ENABLED_DIR));
    }
    std::cout << "Configuration OK: " << apps.size() << " app(s)\n";
}

void terminate_children(const std::vector<AppInstance>& apps, int signal_number) {
    for (const auto& app : apps) {
        if (app.pid > 0) {
            kill(app.pid, signal_number);
        }
    }
}

int run_supervisor() {
    std::vector<AppInstance> apps = load_apps();
    if (apps.empty()) {
        throw std::runtime_error("no enabled apps found in " + std::string(GRANIAN_APPS_ENABLED_DIR));
    }

    install_signal_handlers();

    std::map<pid_t, std::size_t> pid_to_index;
    for (std::size_t index = 0; index < apps.size(); ++index) {
        apps[index].pid = start_child(apps[index]);
        pid_to_index[apps[index].pid] = index;
        std::cerr << "granian-wrapper: started " << apps[index].name
                  << " with pid " << apps[index].pid << "\n";
    }

    while (!pid_to_index.empty()) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) {
                if (g_terminate_requested) {
                    terminate_children(apps, SIGTERM);
                }
                continue;
            }
            throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
        }

        const auto iter = pid_to_index.find(pid);
        if (iter == pid_to_index.end()) {
            continue;
        }

        const std::size_t app_index = iter->second;
        AppInstance& app = apps[app_index];
        pid_to_index.erase(iter);
        app.pid = -1;

        if (g_terminate_requested) {
            continue;
        }

        std::cerr << "granian-wrapper: app " << app.name << " exited";
        if (WIFEXITED(status)) {
            std::cerr << " with status " << WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            std::cerr << " due to signal " << WTERMSIG(status);
        }
        std::cerr << ", restarting\n";

        sleep(2);
        app.pid = start_child(app);
        pid_to_index[app.pid] = app_index;
        std::cerr << "granian-wrapper: restarted " << app.name
                  << " with pid " << app.pid << "\n";
    }

    return 0;
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--check]\n"
        << "Reads global defaults from " << GRANIAN_CONFIG_PATH << " and enabled app configs from "
        << GRANIAN_APPS_ENABLED_DIR << ".\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool check_only = false;
        for (int index = 1; index < argc; ++index) {
            const std::string arg(argv[index]);
            if (arg == "--check") {
                check_only = true;
                continue;
            }
            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            throw std::runtime_error("unknown argument: " + arg);
        }

        if (check_only) {
            validate_apps();
            return 0;
        }
        return run_supervisor();
    } catch (const std::exception& error) {
        std::cerr << "granian-wrapper: " << error.what() << "\n";
        return 1;
    }
}

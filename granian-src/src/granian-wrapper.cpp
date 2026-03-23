#include <algorithm>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
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
bool g_debug_enabled = false;

struct Config {
    std::string app;
    std::string working_directory;
    std::string venv;
    std::string log_dir = "/var/log/granian";
    std::string log_file;
    std::string error_log_file;
    int restart_limit = 5;
    int restart_window = 60;
    int restart_delay = 2;
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
    std::time_t start_time = 0;
    std::vector<std::time_t> recent_failures;
    bool disabled = false;
};

struct ProgramOptions {
    bool check_only = false;
    bool debug = false;
    std::string instance_name;
};

void debug_log(const std::string& message) {
    if (g_debug_enabled) {
        std::cerr << "granian-wrapper[debug]: " << message << "\n";
    }
}

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

bool is_valid_instance_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.') {
            continue;
        }
        return false;
    }
    return true;
}

int parse_positive_integer(const std::string& source, int line_number,
                           const std::string& key, const std::string& raw_value,
                           int min_value) {
    try {
        const int value = std::stoi(trim(raw_value));
        if (value < min_value) {
            throw std::runtime_error(source + ":" + std::to_string(line_number) +
                                     ": " + key + " must be >= " + std::to_string(min_value));
        }
        return value;
    } catch (const std::invalid_argument&) {
        throw std::runtime_error(source + ":" + std::to_string(line_number) +
                                 ": " + key + " must be an integer");
    } catch (const std::out_of_range&) {
        throw std::runtime_error(source + ":" + std::to_string(line_number) +
                                 ": " + key + " is out of range");
    }
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
    if (key == "log-dir") {
        config.log_dir = value;
        return;
    }
    if (key == "log-file") {
        config.log_file = value;
        return;
    }
    if (key == "error-log-file") {
        config.error_log_file = value;
        return;
    }
    if (key == "restart-limit") {
        config.restart_limit = parse_positive_integer(source, line_number, key, value, 1);
        return;
    }
    if (key == "restart-window") {
        config.restart_window = parse_positive_integer(source, line_number, key, value, 1);
        return;
    }
    if (key == "restart-delay") {
        config.restart_delay = parse_positive_integer(source, line_number, key, value, 1);
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
    debug_log("loading config file " + path);
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
    if (g_debug_enabled) {
        if (files.empty()) {
            debug_log("no config fragments found in " + path);
        } else {
            for (const auto& file : files) {
                debug_log("found config fragment " + file);
            }
        }
    }
    return files;
}

void load_optional_config(Config& config, const std::string& config_path, const std::string& config_dir) {
    struct stat statbuf {};
    if (!config_path.empty() && stat(config_path.c_str(), &statbuf) == 0) {
        parse_file(config, config_path);
    } else if (!config_path.empty()) {
        debug_log("config file not present: " + config_path);
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
            std::string cli_key = key;
            if (cli_key == "websockets") {
                cli_key = "ws";
            }
            bool bool_value = false;
            if (parse_bool(value, bool_value)) {
                final_args.emplace_back(bool_value ? "--" + cli_key : "--no-" + cli_key);
                continue;
            }
            final_args.emplace_back("--" + cli_key);
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

std::string default_log_file(const AppInstance& instance) {
    return join_path(instance.config.log_dir, instance.name + ".log");
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
        debug_log("using granian from virtualenv: " + candidate);
        return candidate;
    }
    debug_log("virtualenv granian not found, falling back to " + std::string(GRANIAN_BIN_PATH));
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
    debug_log("activated virtualenv " + config.venv);
}

void apply_env(const Config& config) {
    for (const auto& [name, value] : config.env_vars) {
        if (setenv(name.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("setenv(" + name + ") failed: " + std::strerror(errno));
        }
    }
}

void validate_config(const AppInstance& instance) {
    debug_log("validating app " + instance.name);
    if (instance.config.app.empty()) {
        throw std::runtime_error("missing required 'app=' setting in " + instance.config_path);
    }
    if (!instance.config.working_directory.empty() &&
        !is_directory(instance.config.working_directory)) {
        throw std::runtime_error("working-directory does not exist for " + instance.name +
                                 ": " + instance.config.working_directory);
    }
    if (!instance.config.venv.empty() && !is_directory(instance.config.venv)) {
        throw std::runtime_error("venv directory does not exist for " + instance.name +
                                 ": " + instance.config.venv);
    }
}

int open_log_file(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (fd < 0) {
        throw std::runtime_error("cannot open log file " + path + ": " + std::strerror(errno));
    }
    return fd;
}

void redirect_logs(const AppInstance& instance) {
    const std::string stdout_log = instance.config.log_file.empty()
        ? default_log_file(instance)
        : instance.config.log_file;
    const std::string stderr_log = instance.config.error_log_file.empty()
        ? stdout_log
        : instance.config.error_log_file;

    const int stdout_fd = open_log_file(stdout_log);
    int stderr_fd = stdout_fd;
    if (stderr_log != stdout_log) {
        stderr_fd = open_log_file(stderr_log);
    }

    if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
        throw std::runtime_error("dup2(stdout) failed: " + std::string(std::strerror(errno)));
    }
    if (dup2(stderr_fd, STDERR_FILENO) < 0) {
        throw std::runtime_error("dup2(stderr) failed: " + std::string(std::strerror(errno)));
    }
    debug_log("redirecting app logs for " + instance.name + " to " + stdout_log +
              (stderr_log == stdout_log ? "" : (" and " + stderr_log)));
    if (stdout_fd != STDOUT_FILENO) {
        close(stdout_fd);
    }
    if (stderr_fd != STDERR_FILENO && stderr_fd != stdout_fd) {
        close(stderr_fd);
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
            redirect_logs(instance);
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

AppInstance load_app(const std::string& path, const Config& global_defaults) {
    AppInstance instance;
    instance.name = base_name_without_suffix(path);
    instance.config_path = path;
    instance.config_dir = std::string(GRANIAN_APPS_ENABLED_DIR) + "/" + instance.name + ".d";
    instance.config = global_defaults;
    load_optional_config(instance.config, instance.config_path, instance.config_dir);
    validate_config(instance);
    return instance;
}

std::vector<AppInstance> load_apps(const std::string& instance_name = "") {
    std::vector<AppInstance> apps;
    Config global_defaults;
    debug_log("loading global defaults from " + std::string(GRANIAN_CONFIG_PATH) +
              " and " + std::string(GRANIAN_CONFIG_DIR));
    load_optional_config(global_defaults, GRANIAN_CONFIG_PATH, GRANIAN_CONFIG_DIR);

    if (!instance_name.empty()) {
        if (!is_valid_instance_name(instance_name)) {
            throw std::runtime_error("invalid instance name: " + instance_name);
        }
        const std::string path = std::string(GRANIAN_APPS_ENABLED_DIR) + "/" + instance_name + ".conf";
        struct stat statbuf {};
        if (stat(path.c_str(), &statbuf) != 0) {
            throw std::runtime_error("enabled app config not found: " + path);
        }
        debug_log("loading single app instance " + instance_name + " from " + path);
        apps.push_back(load_app(path, global_defaults));
        return apps;
    }

    debug_log("loading all enabled apps from " + std::string(GRANIAN_APPS_ENABLED_DIR));
    for (const auto& path : list_enabled_apps()) {
        apps.push_back(load_app(path, global_defaults));
    }

    return apps;
}

void validate_apps(const std::string& instance_name = "") {
    const auto apps = load_apps(instance_name);
    if (apps.empty()) {
        throw std::runtime_error("no enabled apps found in " + std::string(GRANIAN_APPS_ENABLED_DIR));
    }
    if (!instance_name.empty()) {
        std::cout << "Configuration OK: " << instance_name << "\n";
        return;
    }
    std::cout << "Configuration OK: " << apps.size() << " app(s)\n";
}

void prune_failures(AppInstance& app, std::time_t now) {
    app.recent_failures.erase(
        std::remove_if(app.recent_failures.begin(), app.recent_failures.end(),
                       [&](std::time_t ts) {
                           return (now - ts) > app.config.restart_window;
                       }),
        app.recent_failures.end());
}

void terminate_children(const std::vector<AppInstance>& apps, int signal_number) {
    for (const auto& app : apps) {
        if (app.pid > 0) {
            kill(app.pid, signal_number);
        }
    }
}

int run_supervisor(const std::string& instance_name = "") {
    std::vector<AppInstance> apps = load_apps(instance_name);
    if (apps.empty()) {
        throw std::runtime_error("no enabled apps found in " + std::string(GRANIAN_APPS_ENABLED_DIR));
    }

    install_signal_handlers();

    std::map<pid_t, std::size_t> pid_to_index;
    for (std::size_t index = 0; index < apps.size(); ++index) {
        debug_log("starting app " + apps[index].name);
        apps[index].pid = start_child(apps[index]);
        apps[index].start_time = std::time(nullptr);
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
        const std::time_t now = std::time(nullptr);

        if (g_terminate_requested) {
            continue;
        }

        std::cerr << "granian-wrapper: app " << app.name << " exited";
        if (WIFEXITED(status)) {
            std::cerr << " with status " << WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            std::cerr << " due to signal " << WTERMSIG(status);
        }
        const std::time_t uptime = app.start_time > 0 ? now - app.start_time : 0;
        if (uptime >= app.config.restart_window) {
            app.recent_failures.clear();
        }
        app.recent_failures.push_back(now);
        prune_failures(app, now);

        if (static_cast<int>(app.recent_failures.size()) >= app.config.restart_limit) {
            app.disabled = true;
            std::cerr << ", restart limit reached; disabling app until service restart\n";
            continue;
        }
        std::cerr << ", restarting\n";

        sleep(app.config.restart_delay);
        debug_log("restart delay elapsed for " + app.name);
        app.pid = start_child(app);
        app.start_time = std::time(nullptr);
        pid_to_index[app.pid] = app_index;
        std::cerr << "granian-wrapper: restarted " << app.name
                  << " with pid " << app.pid << "\n";
    }

    bool any_disabled = false;
    for (const auto& app : apps) {
        any_disabled = any_disabled || app.disabled;
    }
    if (any_disabled) {
        std::cerr << "granian-wrapper: one or more apps were disabled after repeated failures\n";
    }
    return 0;
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [OPTIONS]\n\n"
        << "Granian wrapper and supervisor.\n\n"
        << "Modes:\n"
        << "  default                  manage all enabled apps from " << GRANIAN_APPS_ENABLED_DIR << "\n"
        << "  --instance NAME          manage a single enabled app NAME.conf\n"
        << "  --check                  validate config and exit without starting apps\n\n"
        << "Options:\n"
        << "  --check                  Validate configuration only.\n"
        << "  --instance NAME          Use a single app from apps-enabled/NAME.conf.\n"
        << "  --debug                  Print verbose debug information to stderr.\n"
        << "  -h, --help               Show this help.\n\n"
        << "Wrapper-specific config keys:\n"
        << "  app, working-directory, venv, log-dir, log-file, error-log-file,\n"
        << "  restart-limit, restart-window, restart-delay, env, arg\n\n"
        << "Granian CLI mapping:\n"
        << "  Most other key=value pairs map to Granian CLI flags.\n"
        << "  Booleans become --key/--no-key. websockets maps to --ws/--no-ws.\n";
}

ProgramOptions parse_args(int argc, char** argv) {
    ProgramOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "--check") {
            options.check_only = true;
            continue;
        }
        if (arg == "--debug") {
            options.debug = true;
            continue;
        }
        if (arg == "--instance") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--instance requires an app name");
            }
            options.instance_name = argv[++index];
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        throw std::runtime_error("unknown argument: " + arg);
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const ProgramOptions options = parse_args(argc, argv);
        g_debug_enabled = options.debug;
        if (!options.instance_name.empty()) {
            debug_log("instance mode enabled for " + options.instance_name);
        } else {
            debug_log("global supervisor mode enabled");
        }
        if (options.check_only) {
            debug_log("check-only mode enabled");
        }

        if (options.check_only) {
            validate_apps(options.instance_name);
            return 0;
        }
        return run_supervisor(options.instance_name);
    } catch (const std::exception& error) {
        std::cerr << "granian-wrapper: " << error.what() << "\n";
        return 1;
    }
}

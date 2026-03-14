#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool has_suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_valid_name(const std::string& name) {
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

std::string normalize_name(std::string name) {
    if (!has_suffix(name, ".conf")) {
        name += ".conf";
    }
    if (!is_valid_name(name)) {
        throw std::runtime_error("invalid config name: " + name);
    }
    return name;
}

bool path_exists(const std::string& path) {
    struct stat statbuf {};
    return lstat(path.c_str(), &statbuf) == 0;
}

void usage(const char* argv0) {
#if GRANIAN_ENABLE_CONF
    std::cerr << "Usage: " << argv0 << " APPNAME[.conf]\n";
#else
    std::cerr << "Usage: " << argv0 << " APPNAME[.conf]\n";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            usage(argv[0]);
            return 1;
        }

        const std::string conf_name = normalize_name(argv[1]);
        const std::string available_path = std::string(GRANIAN_APPS_AVAILABLE_DIR) + "/" + conf_name;
        const std::string enabled_path = std::string(GRANIAN_APPS_ENABLED_DIR) + "/" + conf_name;
        const std::string symlink_target = "../apps-available/" + conf_name;

#if GRANIAN_ENABLE_CONF
        if (!path_exists(available_path)) {
            throw std::runtime_error("config not found: " + available_path);
        }
        if (path_exists(enabled_path)) {
            std::cout << conf_name << " already enabled\n";
            return 0;
        }
        if (symlink(symlink_target.c_str(), enabled_path.c_str()) != 0) {
            throw std::runtime_error("cannot enable " + conf_name + ": " + std::strerror(errno));
        }
        std::cout << "Enabled " << conf_name << "\n";
        std::cout << "Run: systemctl reload-or-restart granian.service\n";
#else
        if (!path_exists(enabled_path)) {
            std::cout << conf_name << " already disabled\n";
            return 0;
        }
        if (unlink(enabled_path.c_str()) != 0) {
            throw std::runtime_error("cannot disable " + conf_name + ": " + std::strerror(errno));
        }
        std::cout << "Disabled " << conf_name << "\n";
        std::cout << "Run: systemctl reload-or-restart granian.service\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << argv[0] << ": " << error.what() << "\n";
        return 1;
    }
}

#include "jack_port_renaming.hpp"
#include "jack_wrapper.h"
#include <vector>
#include <regex>
#include <string>
#include <map>

extern jack_port_t* (*jack_port_register_dylibloader_wrapper_jack_orig)( jack_client_t*,const char*,const char*, unsigned long, unsigned long);

struct PortRenameRule {
    std::regex pattern;
    std::string replace_fmt;
}

const std::vector<PortRenameRule> g_shoopdaloop_rules = {
    { std::regex("(.*)"), "better_$&" }
}

std::map<std::string, std::string> active_renames;

// Apply already active renames to a name (faster than a regex every time).
std::string mapped_name(std::string name) {
    auto i = active_renames.find(name);
    if(i != active_renames.end()) {
        return i->second;
    }
    return name;
}

// Apply rewrite rules to a name. If triggered, the rewrite is also stored in the active
// renames map.
std::string apply_rules(std::string name) {
    for (auto const& rule : g_shoopdaloop_rules) {
        std::string result = std::regex_replace(name, rule.pattern, rule.replace_fmt);
        if(result != name) {
            active_renames[name] = result;
            return result;
        }
    }
    return name;
}

jack_port_t* jack_port_register_wrapper(jack_client_t* client, const char* name, const char* type, unsigned long flags, unsigned long buffer_size) {
    return jack_port_register_dylibloader_wrapper_jack_orig(client, apply_rules(name), type, flags, buffer_size);
}

void initialize_shoopdaloop_port_renaming() {
    // Store the original function pointers
    jack_port_register_dylibloader_wrapper_jack_orig = jack_port_register_dylibloader_wrapper_jack;

    // Replace the function pointers by our wrappers
    jack_port_register_dylibloader_wrapper_jack = &jack_port_register_wrapper;
}
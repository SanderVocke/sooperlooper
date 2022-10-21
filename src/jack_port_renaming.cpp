#include "jack_port_renaming.hpp"
#include "jack_wrapper.h"
#include <vector>
#include <regex>
#include <string>
#include <map>
#include <functional>
#include <iostream>
#include <sstream>
#include <exception>
#include <algorithm>
#include <cstring>
#include <memory>

jack_port_t* (*jack_port_register_dylibloader_wrapper_jack_orig)( jack_client_t*,const char*,const char*, unsigned long, unsigned long);
int (*jack_port_unregister_dylibloader_wrapper_jack_orig)( jack_client_t*, jack_port_t*);
void* (*jack_port_get_buffer_dylibloader_wrapper_jack_orig)( jack_port_t*, jack_nframes_t);
jack_nframes_t (*jack_port_get_total_latency_dylibloader_wrapper_jack_orig)( jack_client_t*, jack_port_t*);
int (*jack_set_process_callback_dylibloader_wrapper_jack_orig)( jack_client_t*, JackProcessCallback, void*);

constexpr int loops_per_track = 6;
constexpr bool debug = false;

struct PortRenameRule {
    std::regex pattern;
    std::function<std::string(std::smatch const&)> gen_new_name_fn;
};

struct PortInfo {
    jack_port_t *handle;
    unsigned long flags;
};

std::string gen_track_input_name(std::smatch m) {
    int loop_idx = std::stoi(m[1].str());
    std::ostringstream s;
    if (loop_idx <= 1) {
        // First loop pair is for the master loop.
        s << "master_loop";
    } else {
        // Other pairs are for the tracks.
        s << "track_" << ((loop_idx-2)/2/loops_per_track + 1);
    }
    
    s << std::string((loop_idx % 2) ? "_return_" : "_in_")
      << m[2].str();
    return s.str();
}

std::string gen_track_output_name(std::smatch m) {
    int loop_idx = std::stoi(m[1].str());
    std::ostringstream s;
    if (loop_idx <= 1) {
        // First loop pair is for the master loop.
        s << "master_loop";
    } else {
        // Other pairs are for the tracks.
        s << "track_" << ((loop_idx-2)/2/loops_per_track + 1);
    }
    s << std::string((loop_idx % 2) ? "_out_" : "_send_")
      << m[2].str();
    return s.str();
}

const std::vector<PortRenameRule> g_shoopdaloop_rules = {
    // loop10_in_1 -> track5_in_1
    { std::regex("loop([0-9]+)_in_([1-9])"), gen_track_input_name },
    { std::regex("loop([0-9]+)_out_([1-9])"), gen_track_output_name },
};

struct PortWithInternalBuffer {
    std::vector<jack_default_audio_sample_t> buffer;
};
struct PortWithExternalBuffer {
    jack_default_audio_sample_t* buffer;
};
struct FakeOutputPort : PortWithInternalBuffer {
    jack_port_t* real_port;
};
struct RealOutputPort : PortWithExternalBuffer {};
struct InputPort : PortWithExternalBuffer {};

std::map<jack_port_t*, std::shared_ptr<FakeOutputPort>> fake_output_ports_by_handle;
std::map<std::string, std::shared_ptr<FakeOutputPort>> fake_output_ports_by_name;
std::map<jack_port_t*, std::shared_ptr<RealOutputPort>> real_output_ports_by_handle;
std::map<std::string, std::shared_ptr<RealOutputPort>> real_output_ports_by_name;
std::map<jack_port_t*, std::shared_ptr<InputPort>> input_ports_by_handle;
std::map<std::string, std::shared_ptr<InputPort>> input_ports_by_name;
std::map<std::string, std::string> active_renames;

void* get_fake_output_buffer(jack_port_t* port, jack_nframes_t n_frames) {
    //TODO
}

void* jack_port_get_buffer_wrapper(jack_port_t* port, jack_nframes_t n_frames) {
    auto maybe_fake_output = fake_output_ports_by_handle.find(port);
    if(maybe_fake_output != fake_output_ports_by_handle.end()) {
        return get_fake_output_buffer(port, n_frames);
    }
}

/*
std::map<std::string, std::string> active_renames;
std::map<std::string, PortInfo> active_fake_ports;
std::map<std::string, PortInfo> active_real_ports;
// structure: real port name -> pair of (real output buffer), (map of fake port names to fake output buffers
typedef std::map<std::string, std::vector<jack_default_audio_sample_t>> fake_output_buffers;
std::map<std::string, std::pair<jack_default_audio_sample_t*, fake_output_buffers>> active_output_buffers; // pair of real buffer and associated fakes
std::map<std::string, jack_default_audio_sample_t*> active_input_buffers;
*/

JackProcessCallback process_cb;

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
        std::smatch m;
        if (std::regex_match(name, m, rule.pattern)) {
            std::string new_name = rule.gen_new_name_fn(m);
            return new_name;
        }
    }
    return name;
}

jack_port_t* jack_port_register_wrapper(jack_client_t* client, const char* name, const char* type, unsigned long flags, unsigned long buffer_size) {
    auto mapped_name = apply_rules(name);

    auto p = active_real_ports.find(mapped_name);
    if (p == active_real_ports.end()) {
        if (debug) {
            std::cout << "Jack wrapper: Registering real port for " << name << " (" << mapped_name << ")" << std::endl;
        }
        active_real_ports[mapped_name] = { jack_port_register_dylibloader_wrapper_jack_orig(client, mapped_name.c_str(), type, flags, buffer_size), flags };
    }

    auto f = active_fake_ports.find(name);
    if (f == active_fake_ports.end()) {
        if (debug) {
            std::cout << "Jack wrapper: Creating new fake port for " << name << " (" << mapped_name << ")" << std::endl;
        }
        active_fake_ports[name] = { reinterpret_cast<jack_port_t*>(new int), flags };
    } else {
        if (debug) {
            std::cout << "Jack wrapper: Re-using fake port already instantiated for " << name << " (" << mapped_name << ")" << std::endl;
        }
    }

    active_renames[name] = mapped_name;

    return active_fake_ports[name].handle;
}

int jack_port_unregister_wrapper(jack_client_t* client, jack_port_t* fake_port) {
    // Find the fake port and delete it
    std::string fake_port_name = "";
    for (auto it = active_fake_ports.begin(); it != active_fake_ports.end(); it++) {
        if (it->second.handle == fake_port) {
            fake_port_name = it->first;
            // Delete the fake port
            if (debug) {
                std::cout << "Jack wrapper: Erasing fake port " << fake_port_name << std::endl;
            }
            delete reinterpret_cast<int*>(it->second.handle);
            active_fake_ports.erase(it);
            break;
        }
    }

    if (fake_port_name == "") {
        if (debug) {
            std::cout << "Jack wrapper: unregistering unknown fake port, ignoring" << std::endl;
        }
        return 0;
    }

    // Find the active rename and delete it
    std::string real_port_name = "";
    auto r = active_renames.find(fake_port_name);
    if(r != active_renames.end()) {
        real_port_name = r->second;
        active_renames.erase(r);
    }

    if (real_port_name == "") {
        if (debug) {
            std::cout << "Jack wrapper: deleted fake port " << fake_port_name << " was not associated with any real port" << std::endl;
        }
        return 0;
    }

    // Unregister the real port if no fake ports are left associated to it.
    int fake_ports_left = 0;
    for(auto rr : active_renames) {
        if(rr.second == real_port_name) { fake_ports_left++; }
    }
    if (fake_ports_left == 0) {
        auto rrr = active_real_ports.find(real_port_name);
        if(rrr != active_real_ports.end()) {
            if (debug) {
                std::cout << "Jack wrapper: unregister real port " << real_port_name << std::endl;
            }
            auto retval = jack_port_unregister_dylibloader_wrapper_jack_orig(client, rrr->second.handle);
            if (!retval) {
                active_real_ports.erase(rrr);
            }
            return retval;
        }
    }

    return 0;
}

void* jack_port_get_buffer_wrapper(jack_port_t* port, jack_nframes_t n_frames) {
    // Look up the fake port
    std::string fake_port_name = "";
    for(auto const& rr : active_fake_ports) {
        if (rr.second.handle == port) {
            fake_port_name = rr.first;
            break;
        }
    }

    if (fake_port_name == "") {
        throw std::runtime_error("Jack wrapper: buffer requested for unknown fake port");
    }

    std::string real_port_name = "";
    auto p = active_renames.find(fake_port_name);
    if(p != active_renames.end()) { real_port_name = p->second; }

    if (real_port_name == "") {
        throw std::runtime_error("Jack wrapper: buffer requested for unknown real port");
    }

    auto pp = active_real_ports.find(real_port_name);
    if(pp == active_real_ports.end()) {
        throw std::runtime_error("Jack wrapper: could not find port info for real port");
    }
    auto real_port = pp->second.handle;
    auto flags = pp->second.flags;

    if (flags & JackPortIsOutput) {
        // output port
        // for output ports, we allocate buffers on the fly. when processing finishes,
        // we mix them onto the actual output buffer.
        // however, do make sure we have already requested an output buffer from the
        // real port once.
        auto bb = active_output_buffers.find(real_port_name);
        if (bb == active_output_buffers.end()) {
            active_output_buffers[real_port_name] = { 
                reinterpret_cast<jack_default_audio_sample_t*>(jack_port_get_buffer_dylibloader_wrapper_jack_orig(real_port, n_frames)),
                fake_output_buffers{} };
        }
        auto &fake_output_bufs = active_output_buffers[real_port_name].second;
        auto fb = fake_output_bufs.find(fake_port_name);
        if (fb == fake_output_bufs.end()) {
            fake_output_bufs[fake_port_name] = std::vector<jack_default_audio_sample_t>(n_frames);
        }
        return fake_output_bufs[fake_port_name].data();
    } else {
        // input port
        // for input ports, we just forward the call to the real port the first time.
        // consecutive calls on the same processing run will get the same buffer.
        auto bb = active_input_buffers.find(real_port_name);
        if (bb == active_input_buffers.end()) {
            active_input_buffers[real_port_name] = reinterpret_cast<jack_default_audio_sample_t*>(jack_port_get_buffer_dylibloader_wrapper_jack_orig(real_port, n_frames));
        }
        return active_input_buffers[real_port_name];
    }
}

jack_nframes_t jack_port_get_total_latency_wrapper( jack_client_t* client, jack_port_t* fake_port) {
    // Look up the fake port
    std::string fake_port_name = "";
    for(auto const& rr : active_fake_ports) {
        if (rr.second.handle == fake_port) {
            fake_port_name = rr.first;
            break;
        }
    }

    if (fake_port_name == "") {
        throw std::runtime_error("Jack wrapper: buffer requested for unknown fake port");
    }

    std::string real_port_name = "";
    auto p = active_renames.find(fake_port_name);
    if(p != active_renames.end()) { real_port_name = p->second; }

    if (real_port_name == "") {
        throw std::runtime_error("Jack wrapper: buffer requested for unknown real port");
    }

    auto pp = active_real_ports.find(real_port_name);
    if(pp == active_real_ports.end()) {
        throw std::runtime_error("Jack wrapper: could not find port info for real port");
    }
    auto real_port = pp->second.handle;

    return jack_port_get_total_latency_dylibloader_wrapper_jack_orig(client, real_port);
}

int process_cb_wrapper(jack_nframes_t nframes, void *arg) {
    // Clear any fake buffers we had
    active_input_buffers.clear();
    active_output_buffers.clear();

    auto result = process_cb(nframes, arg);

    // Mix outputs into their real buffer ports
    for(auto const& bb : active_output_buffers) {
        jack_default_audio_sample_t * const& real_buf = bb.second.first;
        fake_output_buffers const& fake_bufs = bb.second.second;
        if (fake_bufs.size() == 0) {
            // No need to write anything
            continue;
        }

        // First buffer is just a straight copy
        memcpy(reinterpret_cast<void*>(real_buf), reinterpret_cast<void const*>(fake_bufs.begin()->second.data()), sizeof(jack_default_audio_sample_t) * nframes);

        // For the rest, do mixing
        auto it = fake_bufs.begin();
        // Skip first element
        for(it++; it != fake_bufs.end(); it++) {
            for(size_t idx=0; idx < nframes; idx++) {
                real_buf[idx] += it->second[idx];
            }
        }
    }

    return result;
}

int jack_set_process_callback_wrapper(jack_client_t* client, JackProcessCallback cb, void* data) {
    process_cb = cb;
    return jack_set_process_callback_dylibloader_wrapper_jack_orig(client, process_cb_wrapper, data);
}

void initialize_shoopdaloop_port_renaming() {
    // Store the original function pointers
    jack_port_register_dylibloader_wrapper_jack_orig = jack_port_register_dylibloader_wrapper_jack;
    jack_port_unregister_dylibloader_wrapper_jack_orig = jack_port_unregister_dylibloader_wrapper_jack;
    jack_port_get_buffer_dylibloader_wrapper_jack_orig = jack_port_get_buffer_dylibloader_wrapper_jack;
    jack_port_get_total_latency_dylibloader_wrapper_jack_orig = jack_port_get_total_latency_dylibloader_wrapper_jack;
    jack_set_process_callback_dylibloader_wrapper_jack_orig = jack_set_process_callback_dylibloader_wrapper_jack;

    // Replace the function pointers by our wrappers
    jack_port_register_dylibloader_wrapper_jack = &jack_port_register_wrapper;
    jack_port_unregister_dylibloader_wrapper_jack = &jack_port_unregister_wrapper;
    jack_port_get_buffer_dylibloader_wrapper_jack = jack_port_get_buffer_wrapper;
    jack_port_get_total_latency_dylibloader_wrapper_jack = jack_port_get_total_latency_wrapper;
    jack_set_process_callback_dylibloader_wrapper_jack = jack_set_process_callback_wrapper;
}
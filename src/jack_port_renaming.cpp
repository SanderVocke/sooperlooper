#include "jack_port_renaming.hpp"
#include "jack_wrapper.h"
#include <jack/types.h>
#include <stdexcept>
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
#include <set>
#include <errno.h>

jack_port_t* (*jack_port_register_dylibloader_wrapper_jack_orig)( jack_client_t*,const char*,const char*, unsigned long, unsigned long);
int (*jack_port_unregister_dylibloader_wrapper_jack_orig)( jack_client_t*, jack_port_t*);
void* (*jack_port_get_buffer_dylibloader_wrapper_jack_orig)( jack_port_t*, jack_nframes_t);
jack_nframes_t (*jack_port_get_total_latency_dylibloader_wrapper_jack_orig)( jack_client_t*, jack_port_t*);
int (*jack_set_process_callback_dylibloader_wrapper_jack_orig)( jack_client_t*, JackProcessCallback, void*);
jack_nframes_t (*jack_midi_get_event_count_dylibloader_wrapper_jack_orig)(void*);
int (*jack_midi_event_get_dylibloader_wrapper_jack_orig)(jack_midi_event_t*, void*, uint32_t);
void (*jack_midi_clear_buffer_dylibloader_wrapper_jack_orig)(void*);
jack_midi_data_t* (*jack_midi_event_reserve_dylibloader_wrapper_jack_orig)(void*, jack_nframes_t, size_t);
int (*jack_midi_event_write_dylibloader_wrapper_jack_orig)(void*, jack_nframes_t, const jack_midi_data_t*, size_t);

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

std::string gen_track_midi_port_name(std::smatch m) {
    int loop_idx = std::stoi(m[1].str());
    std::string in_or_send = m[2].str() == "in" ? "in" : "send";

    std::ostringstream s;
    if (loop_idx <= 1) {
        // First loop pair is for the master loop.
        s << "master_loop";
    } else {
        // Other pairs are for the tracks.
        s << "track_" << ((loop_idx-2)/2/loops_per_track + 1);
    }
    s << "_midi_" << in_or_send;
    return s.str();
}


const std::vector<PortRenameRule> g_shoopdaloop_rules = {
    // loop10_in_1 -> track5_in_1
    { std::regex("loop([0-9]+)_in_([1-9])"), gen_track_input_name },
    { std::regex("loop([0-9]+)_out_([1-9])"), gen_track_output_name },
    { std::regex("loop([0-9]*[02468])_midi_(in|out)"), gen_track_midi_port_name },
    { std::regex("loop([0-9]*[13579])_midi_(in|out)"), [](std::smatch s) { return ""; } }, //No MIDI needed for wet playback loops
};

struct RealOutputPort {
    jack_port_t *handle;
    void* buffer;
    size_t fake_buffers_merged = 0;
    bool midi_cleared = false;
};
struct RealInputPort {
    jack_port_t *handle;
    void* buffer;
};
struct FakeOutputPort  {
    std::vector<jack_default_audio_sample_t> buffer;
    std::shared_ptr<RealOutputPort> real_port;
    bool requested;
    bool hide; // Whether to hide this port internally and make it a "dead end"
    bool is_midi;
};
struct FakeInputPort {
    std::shared_ptr<RealInputPort> real_port;
    bool hide; // Whether to hide this port internally and make it a "dead end"
    bool is_midi;
};

std::map<jack_port_t*, std::shared_ptr<FakeOutputPort>> fake_output_ports_by_handle;
std::map<jack_port_t*, std::shared_ptr<FakeInputPort>> fake_input_ports_by_handle;
std::map<std::string, std::shared_ptr<RealInputPort>> active_real_input_ports;
std::map<std::string, std::shared_ptr<RealOutputPort>> active_real_output_ports;
std::set<void*> hidden_buffers;
std::map<std::string, std::string> active_renames;

void* jack_port_get_buffer_wrapper(jack_port_t* port, jack_nframes_t n_frames) {
    auto maybe_fake_output = fake_output_ports_by_handle.find(port);
    if(maybe_fake_output != fake_output_ports_by_handle.end()) {
        // For every fake output port, there should already be a buffer
        auto &fake_port = *maybe_fake_output->second;

        // Check if we already have a real output buffer to mix to later.
        // If not, request it.
        if (!fake_port.hide) {
            auto &real_port = *fake_port.real_port;
            if (real_port.buffer == NULL) {
                real_port.buffer =
                    jack_port_get_buffer_dylibloader_wrapper_jack_orig(real_port.handle, n_frames);
                real_port.midi_cleared = false;
            }
        }

        if (fake_port.is_midi) {
            // For fake midi outputs, return handle to fake port so we can lookup in
            // MIDI-related wrappers.
            return (void*) &fake_port;
        } else {
            // Return our fake buffer. Resizing should only happen after a buffer size change
            auto &vec = fake_port.buffer;
            if(vec.size() != (size_t)n_frames) { vec.resize((size_t)n_frames); }
            fake_port.requested = true;
            return (void*)vec.data();
        }
    }
    auto maybe_input = fake_input_ports_by_handle.find(port);
    if(maybe_input != fake_input_ports_by_handle.end()) {
        auto &fake_port = *maybe_input->second;

        if (!fake_port.hide) {
            // Request the associated real input buffer if not done yet
            auto &real_port = *fake_port.real_port;
            if (fake_port.real_port->buffer == NULL) {
                fake_port.real_port->buffer = (jack_default_audio_sample_t*)
                    jack_port_get_buffer_dylibloader_wrapper_jack_orig(real_port.handle, n_frames);
            }
        }

        if (fake_port.is_midi && fake_port.hide) {
            // Return the port handle as a buffer handle, so we can identify
            // it for special handling in callbacks
            return (void*) &fake_port;            
        } else {
            // Now return it
            return (void*) fake_port.real_port->buffer;
        }
    }
    
    // If we reach here, the requested buffer was not from a fake port but
    // a real one.
    return jack_port_get_buffer_dylibloader_wrapper_jack_orig(port, n_frames);
}

JackProcessCallback process_cb;

// Apply already active renames to a name (faster than a regex every time).
std::string mapped_name(std::string name) {
    auto i = active_renames.find(name);
    if(i != active_renames.end()) {
        return i->second;
    }
    return name;
}

// Apply rewrite rules to a name.
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

    if(mapped_name == name) {
        // No intercepting for this port
        return jack_port_register_dylibloader_wrapper_jack_orig(client, name, type, flags, buffer_size);
    }

    if (flags & JackPortIsInput) {
        // Create a new fake port associated with the real one and return it
        auto fake_port = std::make_shared<FakeInputPort>();
        fake_port->hide = mapped_name == "";
        fake_port->is_midi = strcmp(type, JACK_DEFAULT_MIDI_TYPE) == 0;

        if (!fake_port->hide) {
            // Create or get the real input port to be associated with our new fake one
            auto it = active_real_input_ports.find(mapped_name);
            if(it == active_real_input_ports.end()) {
                active_real_input_ports[mapped_name] = std::make_shared<RealInputPort>();
                active_real_input_ports[mapped_name]->handle =
                    jack_port_register_dylibloader_wrapper_jack_orig(client, mapped_name.c_str(), type, flags, buffer_size);
            }
            std::shared_ptr<RealInputPort> real_port = active_real_input_ports[mapped_name];
            fake_port->real_port = real_port;
        } else {
            // Mark the returned buffer as a hidden buffer for fast lookup later
            hidden_buffers.insert((void*)fake_port.get());
        }

        jack_port_t *handle = (jack_port_t*)fake_port.get();
        fake_input_ports_by_handle[handle] = fake_port;
        return handle;
    } else if (flags & JackPortIsOutput) {
        // Create a new fake port associated with the real one (including its own buffer) and return it
        auto fake_port = std::make_shared<FakeOutputPort>();
        fake_port->hide = mapped_name == "";
        fake_port->is_midi = strcmp(type, JACK_DEFAULT_MIDI_TYPE) == 0;

        if(!fake_port->hide) {
            // Create or get the real input port to be associated with our new fake one
            auto it = active_real_output_ports.find(mapped_name);
            if(it == active_real_output_ports.end()) {
                active_real_output_ports[mapped_name] = std::make_shared<RealOutputPort>();
                active_real_output_ports[mapped_name]->handle =
                    jack_port_register_dylibloader_wrapper_jack_orig(client, mapped_name.c_str(), type, flags, buffer_size);
            }
            std::shared_ptr<RealOutputPort> real_port = active_real_output_ports[mapped_name];
            fake_port->real_port = real_port;
        } else {
            // Mark the returned buffer as a hidden buffer for fast lookup later
            hidden_buffers.insert((void*)fake_port.get());
        }

        jack_port_t *handle = (jack_port_t*)fake_port.get();
        fake_port->buffer.resize(
            (size_t) jack_port_type_get_buffer_size_dylibloader_wrapper_jack(client, type)
        );
        fake_output_ports_by_handle[handle] = fake_port;
        return handle;
    }

    return jack_port_register_dylibloader_wrapper_jack_orig(client, name, type, flags, buffer_size);
}

int jack_port_unregister_wrapper(jack_client_t* client, jack_port_t* port) {
    auto out = fake_output_ports_by_handle.find(port);
    auto in = fake_input_ports_by_handle.find(port);

    if(out != fake_output_ports_by_handle.end()) {
        fake_output_ports_by_handle.erase(out);
    } else if(in != fake_input_ports_by_handle.end()) {
        fake_input_ports_by_handle.erase(in);
    } else {
        return jack_port_unregister_dylibloader_wrapper_jack_orig(client, port);
    }
    // TODO: active renames update
    return 0;
}

jack_nframes_t jack_port_get_total_latency_wrapper( jack_client_t* client, jack_port_t* port) {
    auto out = fake_output_ports_by_handle.find(port);
    auto in = fake_input_ports_by_handle.find(port);

    if(out != fake_output_ports_by_handle.end()) {
        return out->second->hide ? 0 : jack_port_get_total_latency_dylibloader_wrapper_jack_orig(client, out->second->real_port->handle);
    } else if(in != fake_input_ports_by_handle.end()) {
        return in->second->hide ? 0 : jack_port_get_total_latency_dylibloader_wrapper_jack_orig(client, in->second->real_port->handle);
    } else {
        return jack_port_get_total_latency_dylibloader_wrapper_jack_orig(client, port);
    }
}

jack_nframes_t jack_midi_get_event_count_wrapper(void* buffer) {
    if(hidden_buffers.find(buffer) != hidden_buffers.end()) {
        // Hidden ports never have events
        return 0;
    }

    return jack_midi_get_event_count_dylibloader_wrapper_jack_orig(buffer);
}

int jack_midi_event_get_wrapper(jack_midi_event_t *event, void *buffer, uint32_t event_index) {
    if(hidden_buffers.find(buffer) != hidden_buffers.end()) {
        // Hidden ports never have events.
        return ENODATA;
    }

    return jack_midi_event_get_dylibloader_wrapper_jack_orig(event, buffer, event_index);
}

jack_midi_data_t* jack_midi_event_reserve_wrapper(void *buffer, jack_nframes_t time, size_t data_size) {
    if(hidden_buffers.find(buffer) != hidden_buffers.end()) {
        return nullptr;
    }

    auto it = fake_output_ports_by_handle.find((jack_port_t*)buffer);
    if(it != fake_output_ports_by_handle.end()) {
        // Get events from the associated real port
        return jack_midi_event_reserve_dylibloader_wrapper_jack_orig(it->second->real_port->buffer, time, data_size);
    }

    return jack_midi_event_reserve_dylibloader_wrapper_jack_orig(buffer, time, data_size);
}

int jack_midi_event_write_wrapper(void *buffer, jack_nframes_t time, const jack_midi_data_t* data, size_t data_size) {
    if(hidden_buffers.find(buffer) != hidden_buffers.end()) {
        return ENOBUFS;
    }

    auto it = fake_output_ports_by_handle.find((jack_port_t*)buffer);
    if(it != fake_output_ports_by_handle.end()) {
        // Get events from the associated real port
        return jack_midi_event_write_dylibloader_wrapper_jack_orig(it->second->real_port->buffer, time, data, data_size);
    }

    return jack_midi_event_write_dylibloader_wrapper_jack_orig(buffer, time, data, data_size);
}

void jack_midi_clear_buffer_wrapper(void *buffer) {
    if(hidden_buffers.find(buffer) != hidden_buffers.end()) {
        return;
    }

    auto it = fake_output_ports_by_handle.find((jack_port_t*)buffer);
    if(it != fake_output_ports_by_handle.end()) {
        if(!it->second->real_port->midi_cleared) {
            jack_midi_clear_buffer_dylibloader_wrapper_jack_orig(it->second->real_port->buffer);
            it->second->real_port->midi_cleared = true;
            return;
        }
    }

    jack_midi_clear_buffer_dylibloader_wrapper_jack_orig(buffer);
}

int process_cb_wrapper(jack_nframes_t nframes, void *arg) {
    // Forget any real buffers we had
    for (auto &it : active_real_input_ports) {
        it.second->buffer = NULL;
    }
    for (auto &it : active_real_output_ports) {
        it.second->buffer = NULL;
        it.second->fake_buffers_merged = 0;
    }
    for (auto &it : fake_output_ports_by_handle) {
        it.second->requested = false;
    }

    auto result = process_cb(nframes, arg);

    // Mix outputs into their real buffer ports
    for (auto &it : fake_output_ports_by_handle) {
        if(!it.second->requested || it.second->hide) {
            continue;
        }

        auto &input_buf = it.second->buffer;
        auto &_output_buf = it.second->real_port->buffer;
        auto output_buf = (jack_default_audio_sample_t*) _output_buf;

        if (output_buf == NULL) {
            std::cerr << "Trying to mix into NULL output\n";
            continue;
        }
        
        if (it.second->is_midi) {

        } else {
            // audio port

            // For the first buffer, we just copy the samples
            if (it.second->real_port->fake_buffers_merged++ == 0) {
                memcpy((void*)output_buf, (void*)input_buf.data(), input_buf.size() * sizeof(jack_default_audio_sample_t));
            } else {
                // Mix the samples in
                for(size_t i=0; i<input_buf.size(); i++) {
                    output_buf[i] += input_buf[i];
                }
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
    jack_midi_get_event_count_dylibloader_wrapper_jack_orig = jack_midi_get_event_count_dylibloader_wrapper_jack;
    jack_midi_event_get_dylibloader_wrapper_jack_orig = jack_midi_event_get_dylibloader_wrapper_jack;
    jack_midi_clear_buffer_dylibloader_wrapper_jack_orig = jack_midi_clear_buffer_dylibloader_wrapper_jack;
    jack_midi_event_reserve_dylibloader_wrapper_jack_orig = jack_midi_event_reserve_dylibloader_wrapper_jack;
    jack_midi_event_write_dylibloader_wrapper_jack_orig = jack_midi_event_write_dylibloader_wrapper_jack;

    // Replace the function pointers by our wrappers
    jack_port_register_dylibloader_wrapper_jack = &jack_port_register_wrapper;
    jack_port_unregister_dylibloader_wrapper_jack = &jack_port_unregister_wrapper;
    jack_port_get_buffer_dylibloader_wrapper_jack = jack_port_get_buffer_wrapper;
    jack_port_get_total_latency_dylibloader_wrapper_jack = jack_port_get_total_latency_wrapper;
    jack_set_process_callback_dylibloader_wrapper_jack = jack_set_process_callback_wrapper;
    jack_midi_get_event_count_dylibloader_wrapper_jack = jack_midi_get_event_count_wrapper;
    jack_midi_event_get_dylibloader_wrapper_jack = jack_midi_event_get_wrapper;
    jack_midi_clear_buffer_dylibloader_wrapper_jack = jack_midi_clear_buffer_wrapper;
    jack_midi_event_reserve_dylibloader_wrapper_jack = jack_midi_event_reserve_wrapper;
    jack_midi_event_write_dylibloader_wrapper_jack = jack_midi_event_write_wrapper;
}
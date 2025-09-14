#include <iostream>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>

extern "C" {
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
}

#define BUFFER_FORMAT WL_SHM_FORMAT_XRGB8888
#define PIXEL_SIZE 4

// Color palette (RGB in XRGB8888)
const uint32_t COLORS[][2] = {
    {0x00FF0000, 0x000000FF}, // Red, Blue
    {0x0000FF00, 0x00FFFF00}, // Green, Yellow
    {0x00800080, 0x0000FFFF}, // Purple, Cyan
    {0x00FF00FF, 0x00FFA500}, // Magenta, Orange
};

const int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

class WaylandWindow {
private:
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct xdg_wm_base* wm_base;
    struct wl_shm* shm;

    struct {
        struct wl_surface* surface;
        struct xdg_surface* xdg_surface;
        struct xdg_toplevel* xdg_toplevel;
        struct wl_buffer* buffer;
        struct wl_output* output;
        void* shm_data;
        int width, height;           // ← Now tracked per window
        uint32_t color;
        bool configured = false;
        bool toplevel_configured = false;
        char title[64];
    } windows[2];

    bool running = true;
    std::vector<struct wl_output*> outputs;
    std::vector<std::string> output_names;
    std::vector<int> output_widths;   // ← Store resolutions
    std::vector<int> output_heights;  // ← Store resolutions

    int current_color_index = 0;

    // Output listener callbacks
    static void output_geometry(void* data, struct wl_output* wl_output,
                                int32_t x, int32_t y, int32_t physical_width,
                                int32_t physical_height, int32_t subpixel,
                                const char* make, const char* model,
                                int32_t transform) {}

	static void output_mode(void *data, struct wl_output *wl_output,
							uint32_t flags, int32_t width, int32_t height,
							int32_t refresh)
	{
		WaylandWindow *self = static_cast<WaylandWindow *>(data);

		for (size_t i = 0; i < self->outputs.size(); ++i)
		{
			if (self->outputs[i] == wl_output)
			{
				// Only update if this mode is larger than current
				if (width * height > self->output_widths[i] * self->output_heights[i])
				{
					self->output_widths[i] = width;
					self->output_heights[i] = height;
				}
				return;
			}
		}
	}

    static void output_done(void* data, struct wl_output* wl_output) {}

    static void output_scale(void* data, struct wl_output* wl_output,
                             int32_t factor) {}

    // Registry global handler
    static void registry_global(void* data, struct wl_registry* registry,
                                uint32_t name, const char* interface, uint32_t version) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);

        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
            self->wm_base = static_cast<xdg_wm_base*>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
            xdg_wm_base_add_listener(self->wm_base, &self->wm_base_listener_impl, self);
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->shm = static_cast<wl_shm*>(
                wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
            struct wl_output* output = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, 2)); // v2 for scale/name
            wl_output_add_listener(output, &self->output_listener_impl, self);
            self->outputs.push_back(output);
            self->output_names.push_back("Unknown");
            self->output_widths.push_back(0);   // Initialize resolution trackers
            self->output_heights.push_back(0);
        }
    }

    static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {}

    // xdg_wm_base ping
    static void xdg_wm_base_ping(void* data, struct xdg_wm_base* wm_base, uint32_t serial) {
        xdg_wm_base_pong(wm_base, serial);
    }

    // Generic configure callback
    static void xdg_surface_configure(void* data, struct xdg_surface* surface,
                                      uint32_t serial) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);

        for (int i = 0; i < 2; ++i) {
            if (self->windows[i].xdg_surface == surface) {
                xdg_surface_ack_configure(surface, serial);
                self->windows[i].configured = true;

                if (self->windows[i].configured && self->windows[i].toplevel_configured) {
                    self->create_buffer(i);
                    xdg_toplevel_set_fullscreen(self->windows[i].xdg_toplevel, self->windows[i].output);
                    wl_surface_commit(self->windows[i].surface);
                }
                return;
            }
        }
    }

    // Generic toplevel configure callback
    static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
                                       int32_t width, int32_t height, struct wl_array* states) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);

        for (int i = 0; i < 2; ++i) {
            if (self->windows[i].xdg_toplevel == toplevel) {
                self->windows[i].width = width > 0 ? width : self->windows[i].width;
                self->windows[i].height = height > 0 ? height : self->windows[i].height;
                self->windows[i].toplevel_configured = true;

                if (self->windows[i].configured && self->windows[i].toplevel_configured) {
                    self->create_buffer(i);
                    xdg_toplevel_set_fullscreen(self->windows[i].xdg_toplevel, self->windows[i].output);
                    wl_surface_commit(self->windows[i].surface);
                }
                return;
            }
        }
    }

    // Close window
    static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);
        for (int i = 0; i < 2; ++i) {
            if (self->windows[i].xdg_toplevel == toplevel) {
                std::cout << "❌ Window " << i+1 << " closed.\n";
                self->running = false;
                return;
            }
        }
    }

public:
    WaylandWindow() : display(nullptr), registry(nullptr), compositor(nullptr),
                      wm_base(nullptr), shm(nullptr) {
        for (int i = 0; i < 2; ++i) {
            windows[i].surface = nullptr;
            windows[i].xdg_surface = nullptr;
            windows[i].xdg_toplevel = nullptr;
            windows[i].buffer = nullptr;
            windows[i].output = nullptr;
            windows[i].shm_data = nullptr;
            windows[i].width = 800;
            windows[i].height = 600;
            windows[i].configured = false;
            windows[i].toplevel_configured = false;
            snprintf(windows[i].title, sizeof(windows[i].title), "Window %d", i+1);
        }
    }

    ~WaylandWindow() {
        for (int i = 0; i < 2; ++i) {
            if (windows[i].buffer) wl_buffer_destroy(windows[i].buffer);
            if (windows[i].shm_data) munmap(windows[i].shm_data, windows[i].width * windows[i].height * PIXEL_SIZE);
            if (windows[i].xdg_toplevel) xdg_toplevel_destroy(windows[i].xdg_toplevel);
            if (windows[i].xdg_surface) xdg_surface_destroy(windows[i].xdg_surface);
            if (windows[i].surface) wl_surface_destroy(windows[i].surface);
        }
        if (wm_base) xdg_wm_base_destroy(wm_base);
        if (compositor) wl_compositor_destroy(compositor);
        if (shm) wl_shm_destroy(shm);
        for (auto* out : outputs) wl_output_destroy(out);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
    }

    bool initialize() {
        display = wl_display_connect(nullptr);
        if (!display) {
            std::cerr << "❌ Failed to connect to Wayland display\n";
            return false;
        }

        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener_impl, this);

        //wl_display_roundtrip(display);

		wl_display_dispatch(display);
		wl_display_roundtrip(display);


		if (!compositor || !wm_base || !shm) {
            std::cerr << "❌ Missing required interfaces: compositor, xdg_wm_base, wl_shm\n";
            return false;
        }

        if (outputs.size() < 2) {
            std::cerr << "❌ Need at least 2 monitors. Found: " << outputs.size() << "\n";
            for (size_t i = 0; i < outputs.size(); ++i) {
                std::cerr << "  Output " << i << ": " << output_names[i] << "\n";
            }
            return false;
        }

        // Print monitor resolutions BEFORE creating windows
        std::cout << "\n=== MONITOR RESOLUTIONS ===\n";
        for (size_t i = 0; i < outputs.size(); ++i) {
            int w = output_widths[i];
            int h = output_heights[i];
            if (w == 0 || h == 0) {
                // Fallback: use default if mode not received yet
                w = 1920; h = 1080;
                std::cout << "⚠️  Output " << i << " (" << output_names[i] << ") resolution unknown — using fallback " << w << "x" << h << "\n";
            } else {
                std::cout << "✅ Output " << i << " (" << output_names[i] << "): " << w << "x" << h << "\n";
            }
        }
        std::cout << "=========================\n\n";

        // Assign outputs to windows
        for (int i = 0; i < 2; ++i) {
            windows[i].output = outputs[i];
            // Use detected resolution as initial size
            windows[i].width = output_widths[i] > 0 ? output_widths[i] : 1920;
            windows[i].height = output_heights[i] > 0 ? output_heights[i] : 1080;

            std::cout << "🎯 Window " << i+1 << " assigned to: " << output_names[i] << " (" << windows[i].width << "x" << windows[i].height << ")\n";

            windows[i].surface = wl_compositor_create_surface(compositor);
            if (!windows[i].surface) {
                std::cerr << "❌ Failed to create surface for window " << i+1 << "\n";
                return false;
            }

            windows[i].xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, windows[i].surface);
            xdg_surface_add_listener(windows[i].xdg_surface, &xdg_surface_listener_impl, this);

            windows[i].xdg_toplevel = xdg_surface_get_toplevel(windows[i].xdg_surface);
            xdg_toplevel_add_listener(windows[i].xdg_toplevel, &xdg_toplevel_listener_impl, this);

            xdg_toplevel_set_title(windows[i].xdg_toplevel, windows[i].title);
        }

        // Initialize first colors
        update_colors();

        // Commit surfaces to trigger configure events
        for (int i = 0; i < 2; ++i) {
            wl_surface_commit(windows[i].surface);
            std::cout << "⏳ Waiting for configure events for window " << i+1 << "...\n";
        }

        return true;
    }

    void update_colors() {
        for (int i = 0; i < 2; ++i) {
            windows[i].color = COLORS[current_color_index][i];
        }
        std::cout << "🎨 Changing colors to index " << current_color_index << " — ";
        for (int i = 0; i < 2; ++i) {
            std::cout << "Window " << i+1 << ": " << (windows[i].color == 0x00FF0000 ? "Red" :
                                                     windows[i].color == 0x000000FF ? "Blue" :
                                                     windows[i].color == 0x0000FF00 ? "Green" :
                                                     windows[i].color == 0x00FFFF00 ? "Yellow" :
                                                     windows[i].color == 0x00800080 ? "Purple" :
                                                     windows[i].color == 0x0000FFFF ? "Cyan" :
                                                     windows[i].color == 0x00FF00FF ? "Magenta" :
                                                     windows[i].color == 0x00FFA500 ? "Orange" : "Unknown")
                           << " | ";
        }
        std::cout << "\n";
    }

    void create_buffer(int index) {
        auto& win = windows[index];
        int stride = win.width * PIXEL_SIZE;
        int size = stride * win.height;

        int fd = memfd_create("wayland-buffer", MFD_CLOEXEC);
        if (fd == -1) {
            perror("memfd_create");
            return;
        }

        if (ftruncate(fd, size) == -1) {
            perror("ftruncate");
            close(fd);
            return;
        }

        win.shm_data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (win.shm_data == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return;
        }

        struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
        win.buffer = wl_shm_pool_create_buffer(pool, 0, win.width, win.height, stride, BUFFER_FORMAT);
        wl_shm_pool_destroy(pool);
        close(fd);

        // Fill buffer with assigned color
        uint32_t* pixels = static_cast<uint32_t*>(win.shm_data);
        for (int i = 0; i < win.width * win.height; ++i) {
            pixels[i] = win.color;
        }

        // Attach and damage
        wl_surface_attach(win.surface, win.buffer, 0, 0);
        wl_surface_damage(win.surface, 0, 0, win.width, win.height);

        // Frame callback for smooth presentation
        struct wl_callback* frame_cb = wl_surface_frame(win.surface);
        wl_callback_add_listener(frame_cb, &frame_listener_impl, this);
    }

    void run() {
        std::cout << "▶️ Running Wayland event loop... (close any window to exit)\n";
        std::cout << "⏱️ Colors will change every 3 seconds.\n";

        struct pollfd pfd = {};
        pfd.fd = wl_display_get_fd(display);
        pfd.events = POLLIN;

        while (running) {
            wl_display_dispatch_pending(display);
            wl_display_flush(display);

            int ret = poll(&pfd, 1, 3000);

            if (ret == -1) {
                if (errno == EINTR) continue;
                std::cerr << "❌ poll() failed: " << strerror(errno) << "\n";
                break;
            }

            if (ret == 0) {
                current_color_index = (current_color_index + 1) % NUM_COLORS;
                update_colors();
                for (int i = 0; i < 2; ++i) {
                    create_buffer(i);
                    wl_surface_commit(windows[i].surface);
                }
                wl_display_dispatch_pending(display);
            } else {
                if (wl_display_dispatch(display) == -1) {
                    std::cerr << "❌ wl_display_dispatch() failed\n";
                    break;
                }
            }
        }
    }

private:
    // Output listener
    static constexpr wl_output_listener output_listener_impl = {
        .geometry = output_geometry,
        .mode = output_mode,
        .done = output_done,
        .scale = output_scale
    };

    // Registry listener
    static constexpr wl_registry_listener registry_listener_impl = {
        .global = registry_global,
        .global_remove = registry_global_remove
    };

    // WM base listener
    static constexpr xdg_wm_base_listener wm_base_listener_impl = {
        .ping = xdg_wm_base_ping
    };

    // Surface listener
    static constexpr xdg_surface_listener xdg_surface_listener_impl = {
        .configure = xdg_surface_configure
    };

    // Toplevel listener
    static constexpr xdg_toplevel_listener xdg_toplevel_listener_impl = {
        .configure = xdg_toplevel_configure,
        .close = xdg_toplevel_close
    };

    // Frame callback listener
    static void frame_callback(void* data, struct wl_callback* callback, uint32_t time) {
        wl_callback_destroy(callback);
    }

    static constexpr wl_callback_listener frame_listener_impl = {
        .done = frame_callback
    };
};

int main(int argc, char** argv) {
    WaylandWindow window;

    if (!window.initialize()) {
        return 1;
    }

    window.run();
    return 0;
}
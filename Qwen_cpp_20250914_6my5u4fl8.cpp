#include <iostream>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>

extern "C" {
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
}

#define BUFFER_FORMAT WL_SHM_FORMAT_XRGB8888
#define PIXEL_SIZE 4

class WaylandWindow {
private:
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_compositor* compositor;
    struct xdg_wm_base* wm_base;
    struct wl_surface* surface;
    struct xdg_surface* xdg_surface;
    struct xdg_toplevel* xdg_toplevel;
    struct wl_shm* shm;
    struct wl_buffer* buffer;
    struct wl_output* target_output;

    int width = 800;
    int height = 600;
    bool running = true;
    void* shm_data;

    // State flags
    bool xdg_surface_configured = false;
    bool xdg_toplevel_configured = false;

    std::vector<struct wl_output*> outputs;
    std::vector<std::string> output_names;

    // Output listener callbacks
    static void output_geometry(void* data, struct wl_output* wl_output,
                                int32_t x, int32_t y, int32_t physical_width,
                                int32_t physical_height, int32_t subpixel,
                                const char* make, const char* model,
                                int32_t transform) {}

    static void output_mode(void* data, struct wl_output* wl_output,
                            uint32_t flags, int32_t width, int32_t height,
                            int32_t refresh) {}

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
        }
    }

    static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {}

    // xdg_wm_base ping
    static void xdg_wm_base_ping(void* data, struct xdg_wm_base* wm_base, uint32_t serial) {
        xdg_wm_base_pong(wm_base, serial);
    }

    // xdg_surface configure
    static void xdg_surface_configure(void* data, struct xdg_surface* surface,
                                      uint32_t serial) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);
        xdg_surface_ack_configure(surface, serial);
        self->xdg_surface_configured = true;

        // Only proceed if both surfaces are configured
        if (self->xdg_surface_configured && self->xdg_toplevel_configured) {
            self->create_buffer();
            // Now safe to set fullscreen
            xdg_toplevel_set_fullscreen(self->xdg_toplevel, self->target_output);
            std::cout << "✅ Fullscreen requested on: " << self->output_names[1] << "\n";
            wl_surface_commit(self->surface);
        }
    }

    // xdg_toplevel configure
    static void xdg_toplevel_configure(void* data, struct xdg_toplevel* toplevel,
                                       int32_t width, int32_t height, struct wl_array* states) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);
        self->width = width > 0 ? width : self->width;
        self->height = height > 0 ? height : self->height;
        self->xdg_toplevel_configured = true;

        // Only create buffer when both are configured
        if (self->xdg_surface_configured && self->xdg_toplevel_configured) {
            self->create_buffer();
            xdg_toplevel_set_fullscreen(self->xdg_toplevel, self->target_output);
            std::cout << "✅ Both configure events received. Setting fullscreen.\n";
            wl_surface_commit(self->surface);
        }
    }

    // Close window
    static void xdg_toplevel_close(void* data, struct xdg_toplevel* toplevel) {
        WaylandWindow* self = static_cast<WaylandWindow*>(data);
        self->running = false;
    }

public:
    WaylandWindow() : display(nullptr), registry(nullptr), compositor(nullptr),
                      wm_base(nullptr), surface(nullptr), xdg_surface(nullptr),
                      xdg_toplevel(nullptr), shm(nullptr), buffer(nullptr),
                      target_output(nullptr), shm_data(nullptr),
                      xdg_surface_configured(false), xdg_toplevel_configured(false) {}

    ~WaylandWindow() {
        if (buffer) wl_buffer_destroy(buffer);
        if (shm_data) munmap(shm_data, width * height * PIXEL_SIZE);
        if (xdg_toplevel) xdg_toplevel_destroy(xdg_toplevel);
        if (xdg_surface) xdg_surface_destroy(xdg_surface);
        if (surface) wl_surface_destroy(surface);
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

        // Wait for all globals (including outputs)
        wl_display_roundtrip(display);

        if (!compositor || !wm_base || !shm) {
            std::cerr << "❌ Missing required interfaces: compositor, xdg_wm_base, wl_shm\n";
            return false;
        }

        // Must have at least 2 outputs
        if (outputs.size() < 2) {
            std::cerr << "❌ Need at least 2 monitors. Found: " << outputs.size() << "\n";
            for (size_t i = 0; i < outputs.size(); ++i) {
                std::cerr << "  Output " << i << ": " << output_names[i] << "\n";
            }
            return false;
        }

        target_output = outputs[1];
        std::cout << "🎯 Selected second output: " << output_names[1] << "\n";

        // Create surface
        surface = wl_compositor_create_surface(compositor);
        if (!surface) {
            std::cerr << "❌ Failed to create surface\n";
            return false;
        }

        xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
        xdg_surface_add_listener(xdg_surface, &xdg_surface_listener_impl, this);

        xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
        xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener_impl, this);

        xdg_toplevel_set_title(xdg_toplevel, "Green Fullscreen on Second Monitor");

        // Commit to trigger initial configure events
        wl_surface_commit(surface);

        std::cout << "⏳ Waiting for configure events...\n";
        return true;
    }

    void create_buffer() {
        int stride = width * PIXEL_SIZE;
        int size = stride * height;

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

        shm_data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (shm_data == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return;
        }

        struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
        buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, BUFFER_FORMAT);
        wl_shm_pool_destroy(pool);
        close(fd);

        // Fill with GREEN: 0x0000FF00 (XRGB8888)
        uint32_t* pixels = static_cast<uint32_t*>(shm_data);
        for (int i = 0; i < width * height; ++i) {
            pixels[i] = 0x0000FF00; // Green
        }

        // Attach and damage
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, width, height);

        // Optional: Use frame callback to ensure presentation
        struct wl_callback* frame_cb = wl_surface_frame(surface);
        wl_callback_add_listener(frame_cb, &frame_listener_impl, this);

        wl_surface_commit(surface);
        std::cout << "🎨 Buffer attached and committed.\n";
    }

    void run() {
        std::cout << "▶️ Running Wayland event loop... (close window to exit)\n";

        while (running) {
            if (wl_display_dispatch(display) == -1) {
                break;
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

    // Frame callback listener — ensures rendering is presented
    static void frame_callback(void* data, struct wl_callback* callback, uint32_t time) {
        wl_callback_destroy(callback);
        // We don't need to do anything here — just used to ensure paint happens
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
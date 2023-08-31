#define _POSIX_C_SOURCE 200112L
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_pointer.h>

enum miniwl_cursor_mode
{
    MINIWL_CURSOR_PASSTHROUGH,
    MINIWL_CURSOR_MOVE,
    MINIWL_CURSOR_RESIZE,
};

struct miniwl_server
{
    /* data */
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_surface;
    struct wl_list views;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wl_list keyboard;
    enum miniwl_cursor_mode cursor_mode;
    struct miniwl_view *grabbed_view;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
};

struct miniwl_view
{
    struct wl_list link;
    struct miniwl_server *server;
    struct wlr_xdg_surface *xdg_surface;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    bool mapped;
    int x, y;
};

struct miniwl_output
{
    struct wl_list link;
    struct miniwl_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
};

struct miniwl_keyboard
{
    struct wl_list link;
    struct miniwl_server *server;
    struct wlr_input_device *device;
    struct wl_listener modifiers;
    struct wl_listener key;
};

struct render_data
{
    struct wlr_output *output;
    struct wlr_renderer *renderer;
    struct miniwl_view *view;
    struct timespec *when;
};

static bool view_at(struct miniwl_view *view, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);
static void output_frame(struct wl_listener *listener, void *data);
static void server_new_output(struct wl_listener *listener, void *data);
static void server_new_input(struct wl_listener *listener, void *data);
static void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
static bool handle_keybinding(struct miniwl_server *server, xkb_keysym_t sym);
static void keyboard_handle_key(struct wl_listener *listener, void *data);
static void server_new_keyboard(struct miniwl_server *server, struct wlr_input_device *device);
static void server_new_pointer(struct miniwl_server *server, struct wlr_input_device *device);
static void render_surface(struct wlr_surface *surface, int sx, int sy, void *data);
static void focus_view(struct miniwl_view *view, struct wlr_surface *surface);
static void xdg_surface_map(struct wl_listener *listener, void *data);
static void xdg_surface_unmap(struct wl_listener *listener, void *data);
static void xdg_surface_destroy(struct wl_listener *listener, void *data);
static void server_new_xdg_surface(struct wl_listener *listener, void *data);
static void server_cursor_motion(struct wl_listener *listener, void *data);
static void process_cursor_motion(struct miniwl_server *server, uint32_t time);
static void process_cursor_move(struct miniwl_server *server, uint32_t time);
static void process_cursor_resize(struct miniwl_server *server, uint32_t time);
static void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
static void server_cursor_button(struct wl_listener *listener, void *data);
static void server_cursor_axis(struct wl_listener *listener, void *data);
static void server_cursor_frame(struct wl_listener *listener, void *data);
static void seat_request_cursor(struct wl_listener *listener, void *data);
static void seat_request_set_selection(struct wl_listener *listener, void *data);


static bool view_at(struct miniwl_view *view,
        double lx, double ly, struct wlr_surface **surface,
                double *sx, double *sy)
{
    double view_sx = lx - view->x;
    double view_sy = ly - view_sy;

    double _sx, _sy;
    struct wlr_surface *_surface = NULL;
    _surface = wlr_xdg_surface_surface_at(view->xdg_surface, view_sx, view_sy, &_sx, &_sy);

    if (_surface != NULL)
    {
        *sx = _sx;
        *sy = _sy;
        *surface = _surface;
        return true;
    }
    return false;
}

static struct miniwl_view *desktop_view_at(
        struct miniwl_server *server, double lx, double ly,
                struct wlr_surface **surface, double *sx, double *sy
        )
{
    struct miniwl_view *view;
    wl_list_for_each(view, &server->views, link)
    {
        if (view_at(view, lx, ly, surface, sx, sy))
            return view;
    }
    return NULL;
}

static void server_new_output(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    if (!wl_list_empty(&wlr_output->modes))
    {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
        wlr_output_set_mode(wlr_output, mode);
        wlr_output_enable(wlr_output, true);
        if (!wlr_output_commit(wlr_output))
        {
            return;
        }
    }

    struct miniwl_output *output = calloc(1, sizeof(struct miniwl_output));
    output->wlr_output = wlr_output;
    output->server = server;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    wl_list_insert(&server->outputs, &output->link);
    wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            server_new_keyboard(server, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            server_new_pointer(server, device);
            break;
        default:
            break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboard))
    {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
    struct miniwl_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, wlr_keyboard_from_input_device(keyboard->device));
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &wlr_keyboard_from_input_device(keyboard->device)->modifiers);
}

static bool handle_keybinding(struct miniwl_server *server, xkb_keysym_t sym)
{
    switch (sym) {
        case XKB_KEY_Escape:
        wl_display_terminate(server->wl_display);
        break;
        case XKB_KEY_F1:
            if (wl_list_length(&server->views) < 2)
            {
                break;
            }
            struct miniwl_view *current_view = wl_container_of(server->views.next, current_view, link);
            struct miniwl_view *next_view = wl_container_of(current_view->link.next, next_view, link);
            focus_view(next_view, next_view->xdg_surface->surface);
            wl_list_remove(&current_view->link);
            wl_list_insert(server->views.prev, &current_view->link);
            break;
        default:
            return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
    struct miniwl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct miniwl_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(wlr_keyboard_from_input_device(keyboard->device)->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_keyboard_from_input_device(keyboard->device));
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_RELEASED)
    {
        for (int i = 0; i < nsyms; i++)
        {
            handled = handle_keybinding(server, syms[i]);
        }

        if (!handled)
        {
            wlr_seat_set_keyboard(seat, wlr_keyboard_from_input_device(keyboard->device));
            wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        }
    }
}

static void server_new_keyboard(struct miniwl_server *server, struct wlr_input_device *device)
{
    struct miniwl_keyboard *keyboard = calloc(1, sizeof(struct miniwl_keyboard));
    keyboard->server = server;
    keyboard->device = device;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(wlr_keyboard_from_input_device(device), keymap);
    xkb_keymap_ref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard_from_input_device(device), 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard_from_input_device(device)->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
}

static void server_new_pointer(struct miniwl_server *server, struct wlr_input_device *device)
{
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void render_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
    struct render_data *rdata = data;
    struct miniwl_view *view = rdata->view;
    struct wlr_output *output = rdata->output;

    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (texture == NULL)
    {
        return ;
    }

    double ox = 0, oy = 0;
    wlr_output_layout_output_coords(view->server->output_layout, output, &ox, &oy);
    ox == view->x + sx;
    oy += view->y + sy;

    struct wlr_box box = {
            .x = ox * output->scale,
            .y = oy * output->scale,
            .width = surface->current.width * output->scale,
            .height = surface->current.height * output->scale,
    };

    float matrix[9] = {0};
    enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
    wlr_matrix_project_box(matrix, &box, transform, 0, output->transform_matrix);
    wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);
    wlr_surface_send_frame_done(surface, rdata->when);
}

static void focus_view(struct miniwl_view *view, struct wlr_surface *surface)
{
    if (view == NULL)
    {
        return ;
    }
    struct miniwl_server *server = view->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface)
    {
        return ;
    }
    if (prev_surface)
    {
        struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(seat->keyboard_state.focused_surface);
        wlr_xdg_toplevel_set_activated(previous->toplevel, false);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);

    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    wlr_xdg_toplevel_set_activated(view->xdg_surface->toplevel, true);
    wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static void xdg_surface_map(struct wl_listener *listener, void *data)
{
    struct miniwl_view *view = wl_container_of(listener, view, map);
    view->mapped = true;
    focus_view(view, view->xdg_surface->surface);
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data)
{
    struct miniwl_view *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data)
{
    struct miniwl_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

static void output_frame(struct wl_listener *listener, void *data)
{
    struct miniwl_output *output = wl_container_of(listener, output, frame);
    struct wlr_renderer *renderer = output->server->renderer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!wlr_output_attach_render(output->wlr_output, NULL))
    {
        return;
    }
    int width, height;
    wlr_output_effective_resolution(output->wlr_output, &width, &height);
    wlr_renderer_begin(renderer, width, height);

    float color[4] = {0.3, 0.3, 0.3, 1.0};
    wlr_renderer_clear(renderer, color);
    struct miniwl_view *view;
    wl_list_for_each_reverse(view, &output->server->views, link)
    {
        if (!view->mapped)
        {
            continue;
        }
        struct render_data rdata = {
                .output = output->wlr_output,
                .view = view,
                .renderer = renderer,
                .when = &now,
        };
        wlr_xdg_surface_for_each_surface(view->xdg_surface, render_surface, &rdata);
    }

    wlr_output_render_software_cursors(output->wlr_output, NULL);
    wlr_renderer_end(renderer);
    wlr_output_commit(output->wlr_output);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
    {
        return ;
    }

    struct miniwl_view *view = calloc(1, sizeof(struct miniwl_view));
    view->server = server;
    view->xdg_surface = xdg_surface;

    view->map.notify = xdg_surface_map;
}

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void process_cursor_motion(struct miniwl_server *server, uint32_t time)
{
    if (server->cursor_mode == MINIWL_CURSOR_MOVE)
    {
        process_cursor_move(server, time);
        return;
    }
    else if (server->cursor_mode == MINIWL_CURSOR_RESIZE)
    {
        process_cursor_resize(server, time);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct miniwl_view *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (!view)
    {
        wlr_xcursor_manager_set_cursor_image(server->cursor_mgr, "left_ptr", server->cursor);
    }

    if (surface)
    {
        wlr_seat_pointer_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    else
    {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void process_cursor_move(struct miniwl_server *server, uint32_t time)
{
    server->grabbed_view->x = server->cursor->x - server->grab_x;
    server->grabbed_view->y = server->cursor->y - server->grab_y;
}

static void process_cursor_resize(struct miniwl_server *server, uint32_t time)
{
    struct miniwl_view *view = server->grabbed_view;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP)
    {
        new_top = border_y;
        if (new_top >= new_bottom)
        {
            new_top = new_bottom - 1;
        }
        else if (server->resize_edges & WLR_EDGE_BOTTOM)
        {
            new_bottom = border_y;
            if (new_bottom <= new_top)
            {
                new_bottom = new_top + 1;
            }
        }
        if (server->resize_edges & WLR_EDGE_LEFT)
        {
            new_left = border_x;
            if (new_left >= new_left)
            {
                new_left = new_right + 1;
            }
        }
        else if (server->resize_edges & WLR_EDGE_RIGHT)
        {
            new_right = border_x;
            if (new_right <= new_left)
            {
                new_right = new_left + 1;
            }
        }

        struct wlr_box geo_box;
        wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);
        view->x = new_left - geo_box.x;
        view->y = new_top - geo_box.y;

        int new_width = new_right;
        int new_height = new_bottom - new_top;
        wlr_xdg_toplevel_set_size(view->xdg_surface->toplevel, new_width, new_height);
    }
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    double sx, sy;
    struct wlr_surface *surface;
    struct miniwl_view *view = desktop_view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (event->state == WLR_BUTTON_RELEASED)
    {
        server->cursor_mode = MINIWL_CURSOR_PASSTHROUGH;
    }
    else
    {
        focus_view(view, surface);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client)
    {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
    struct miniwl_server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

int main(int argc, char *argv[])
{
    wlr_log_init(WLR_DEBUG, NULL);
    char *startup_cmd = NULL;

    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1)
    {
        switch (c)
        {
            case 's':
                startup_cmd = optarg;
                break;
            default:
                printf("Usage: %s [-s startup command]\n", argv[0]);
                return 0;
        }
    }

    if (optind < argc)
    {
        printf("Usage: %s [-s startup command]\n", argv[0]);
        return 0;
    }

    struct miniwl_server server;
    server.wl_display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.wl_display);
    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);
    wlr_compositor_create(server.wl_display, server.renderer);
    wlr_data_device_manager_create(server.wl_display);

    server.output_layout = wlr_output_layout_create();

    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    wl_list_init(&server.views);
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 1);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);
    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboard);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (socket)
    {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend))
    {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd)
    {
        if (fork() == 0)
        {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
        }
    }

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.wl_display);
    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);
    return 0;
}

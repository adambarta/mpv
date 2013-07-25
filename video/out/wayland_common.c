/*
 * This file is part of MPlayer.
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012-2013 Collabora, Ltd.
 * Copyright © 2012-2013 Scott Moreau <oreaus@gmail.com>
 * Copyright © 2012-2013 Alexander Preisinger <alexander.preisinger@gmail.com>
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>
#include <poll.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/input.h>

#include "config.h"
#include "core/bstr.h"
#include "core/options.h"
#include "core/mp_msg.h"
#include "libavutil/common.h"
#include "talloc.h"

#include "wayland_common.h"

#include "vo.h"
#include "aspect.h"
#include "osdep/timer.h"

#include "core/input/input.h"
#include "core/input/keycodes.h"

#define MOD_SHIFT_MASK      0x01
#define MOD_ALT_MASK        0x02
#define MOD_CONTROL_MASK    0x04

static int lookupkey(int key);

static void hide_cursor(struct vo_wayland_state * wl);
static void show_cursor(struct vo_wayland_state * wl);
static void resize_window(struct vo_wayland_state *wl,
                          uint32_t edges,
                          int32_t width,
                          int32_t height);

static void vo_wayland_fullscreen (struct vo *vo);

static const struct mp_keymap keymap[] = {
    // special keys
    {XKB_KEY_Pause,     MP_KEY_PAUSE}, {XKB_KEY_Escape, MP_KEY_ESC},
    {XKB_KEY_BackSpace, MP_KEY_BS},    {XKB_KEY_Tab,    MP_KEY_TAB},
    {XKB_KEY_Return,    MP_KEY_ENTER}, {XKB_KEY_Menu,   MP_KEY_MENU},
    {XKB_KEY_Print,     MP_KEY_PRINT},

    // cursor keys
    {XKB_KEY_Left, MP_KEY_LEFT}, {XKB_KEY_Right, MP_KEY_RIGHT},
    {XKB_KEY_Up,   MP_KEY_UP},   {XKB_KEY_Down,  MP_KEY_DOWN},

    // navigation block
    {XKB_KEY_Insert,  MP_KEY_INSERT},  {XKB_KEY_Delete,    MP_KEY_DELETE},
    {XKB_KEY_Home,    MP_KEY_HOME},    {XKB_KEY_End,       MP_KEY_END},
    {XKB_KEY_Page_Up, MP_KEY_PAGE_UP}, {XKB_KEY_Page_Down, MP_KEY_PAGE_DOWN},

    // F-keys
    {XKB_KEY_F1,  MP_KEY_F+1},  {XKB_KEY_F2,  MP_KEY_F+2},
    {XKB_KEY_F3,  MP_KEY_F+3},  {XKB_KEY_F4,  MP_KEY_F+4},
    {XKB_KEY_F5,  MP_KEY_F+5},  {XKB_KEY_F6,  MP_KEY_F+6},
    {XKB_KEY_F7,  MP_KEY_F+7},  {XKB_KEY_F8,  MP_KEY_F+8},
    {XKB_KEY_F9,  MP_KEY_F+9},  {XKB_KEY_F10, MP_KEY_F+10},
    {XKB_KEY_F11, MP_KEY_F+11}, {XKB_KEY_F12, MP_KEY_F+12},

    // numpad independent of numlock
    {XKB_KEY_KP_Subtract, '-'}, {XKB_KEY_KP_Add, '+'},
    {XKB_KEY_KP_Multiply, '*'}, {XKB_KEY_KP_Divide, '/'},
    {XKB_KEY_KP_Enter, MP_KEY_KPENTER},

    // numpad with numlock
    {XKB_KEY_KP_0, MP_KEY_KP0}, {XKB_KEY_KP_1, MP_KEY_KP1},
    {XKB_KEY_KP_2, MP_KEY_KP2}, {XKB_KEY_KP_3, MP_KEY_KP3},
    {XKB_KEY_KP_4, MP_KEY_KP4}, {XKB_KEY_KP_5, MP_KEY_KP5},
    {XKB_KEY_KP_6, MP_KEY_KP6}, {XKB_KEY_KP_7, MP_KEY_KP7},
    {XKB_KEY_KP_8, MP_KEY_KP8}, {XKB_KEY_KP_9, MP_KEY_KP9},
    {XKB_KEY_KP_Decimal, MP_KEY_KPDEC}, {XKB_KEY_KP_Separator, MP_KEY_KPDEC},

    // numpad without numlock
    {XKB_KEY_KP_Insert, MP_KEY_KPINS}, {XKB_KEY_KP_End,       MP_KEY_KP1},
    {XKB_KEY_KP_Down,   MP_KEY_KP2},   {XKB_KEY_KP_Page_Down, MP_KEY_KP3},
    {XKB_KEY_KP_Left,   MP_KEY_KP4},   {XKB_KEY_KP_Begin,     MP_KEY_KP5},
    {XKB_KEY_KP_Right,  MP_KEY_KP6},   {XKB_KEY_KP_Home,      MP_KEY_KP7},
    {XKB_KEY_KP_Up,     MP_KEY_KP8},   {XKB_KEY_KP_Page_Up,   MP_KEY_KP9},
    {XKB_KEY_KP_Delete, MP_KEY_KPDEL},

    {0, 0}
};


/** Wayland listeners **/
static void ssurface_handle_ping(void *data,
                                 struct wl_shell_surface *shell_surface,
                                 uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void ssurface_handle_configure(void *data,
                                      struct wl_shell_surface *shell_surface,
                                      uint32_t edges,
                                      int32_t width,
                                      int32_t height)
{
    struct vo_wayland_state *wl = data;
    resize_window(wl, edges, width, height);
}

static void ssurface_handle_popup_done(void *data,
                                       struct wl_shell_surface *shell_surface)
{
}

const struct wl_shell_surface_listener shell_surface_listener = {
    ssurface_handle_ping,
    ssurface_handle_configure,
    ssurface_handle_popup_done
};

static void output_handle_geometry(void *data,
                                   struct wl_output *wl_output,
                                   int32_t x,
                                   int32_t y,
                                   int32_t physical_width,
                                   int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make,
                                   const char *model,
                                   int32_t transform)
{
    /* Ignore transforms for now */
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_NORMAL:
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_180:
        case WL_OUTPUT_TRANSFORM_270:
        case WL_OUTPUT_TRANSFORM_FLIPPED:
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        default:
            break;
    }
}

static void output_handle_mode(void *data,
                               struct wl_output *wl_output,
                               uint32_t flags,
                               int32_t width,
                               int32_t height,
                               int32_t refresh)
{
    struct vo_wayland_output *output = data;

    if (!output)
        return;

    output->width = width;
    output->height = height;
    output->flags = flags;
}

const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode
};

/* KEYBOARD LISTENER */
static void keyboard_handle_keymap(void *data,
                                   struct wl_keyboard *wl_keyboard,
                                   uint32_t format,
                                   int32_t fd,
                                   uint32_t size)
{
    struct vo_wayland_state *wl = data;
    char *map_str;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    wl->input.xkb.keymap = xkb_keymap_new_from_buffer(wl->input.xkb.context,
                                                      map_str,
                                                      size,
                                                      XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      0);

    munmap(map_str, size);
    close(fd);

    if (!wl->input.xkb.keymap) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wayland] failed to compile keymap.\n");
        return;
    }

    wl->input.xkb.state = xkb_state_new(wl->input.xkb.keymap);
    if (!wl->input.xkb.state) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wayland] failed to create XKB state.\n");
        xkb_map_unref(wl->input.xkb.keymap);
        wl->input.xkb.keymap = NULL;
        return;
    }
}

static void keyboard_handle_enter(void *data,
                                  struct wl_keyboard *wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface *surface,
                                  struct wl_array *keys)
{
}

static void keyboard_handle_leave(void *data,
                                  struct wl_keyboard *wl_keyboard,
                                  uint32_t serial,
                                  struct wl_surface *surface)
{
}

static void keyboard_handle_key(void *data,
                                struct wl_keyboard *wl_keyboard,
                                uint32_t serial,
                                uint32_t time,
                                uint32_t key,
                                uint32_t state)
{
    struct vo_wayland_state *wl = data;
    uint32_t code, num_syms;
    int mpkey;

    const xkb_keysym_t *syms;
    xkb_keysym_t sym;

    code = key + 8;
    num_syms = xkb_key_get_syms(wl->input.xkb.state, code, &syms);

    sym = XKB_KEY_NoSymbol;
    if (num_syms == 1)
        sym = syms[0];

    if (sym != XKB_KEY_NoSymbol && (mpkey = lookupkey(sym))) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            mp_input_put_key(wl->vo->input_ctx, mpkey | MP_KEY_STATE_DOWN);
        else
            mp_input_put_key(wl->vo->input_ctx, mpkey | MP_KEY_STATE_UP);
    }
}

static void keyboard_handle_modifiers(void *data,
                                      struct wl_keyboard *wl_keyboard,
                                      uint32_t serial,
                                      uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked,
                                      uint32_t group)
{
    struct vo_wayland_state *wl = data;

    xkb_state_update_mask(wl->input.xkb.state,
                          mods_depressed,
                          mods_latched,
                          mods_locked,
                          0, 0, group);
}

const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers
};

/* POINTER LISTENER */
static void pointer_handle_enter(void *data,
                                 struct wl_pointer *pointer,
                                 uint32_t serial,
                                 struct wl_surface *surface,
                                 wl_fixed_t sx_w,
                                 wl_fixed_t sy_w)
{
    struct vo_wayland_state *wl = data;

    wl->cursor.serial = serial;
    wl->cursor.pointer = pointer;

    /* Release the left button on pointer enter again
     * because after moving the shell surface no release event is sent */
    mp_input_put_key(wl->vo->input_ctx, MP_MOUSE_BTN0 | MP_KEY_STATE_UP);
    show_cursor(wl);
}

static void pointer_handle_leave(void *data,
                                 struct wl_pointer *pointer,
                                 uint32_t serial,
                                 struct wl_surface *surface)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_MOUSE_LEAVE);
}

static void pointer_handle_motion(void *data,
                                  struct wl_pointer *pointer,
                                  uint32_t time,
                                  wl_fixed_t sx_w,
                                  wl_fixed_t sy_w)
{
    struct vo_wayland_state *wl = data;

    wl->cursor.pointer = pointer;

    vo_mouse_movement(wl->vo, wl_fixed_to_int(sx_w),
                              wl_fixed_to_int(sy_w));
}

static void pointer_handle_button(void *data,
                                  struct wl_pointer *pointer,
                                  uint32_t serial,
                                  uint32_t time,
                                  uint32_t button,
                                  uint32_t state)
{
    struct vo_wayland_state *wl = data;

    mp_input_put_key(wl->vo->input_ctx, MP_MOUSE_BTN0 + (button - BTN_LEFT) |
                    ((state == WL_POINTER_BUTTON_STATE_PRESSED)
                    ? MP_KEY_STATE_DOWN : MP_KEY_STATE_UP));

    if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        wl_shell_surface_move(wl->window.shell_surface, wl->input.seat, serial);
}

static void pointer_handle_axis(void *data,
                                struct wl_pointer *pointer,
                                uint32_t time,
                                uint32_t axis,
                                wl_fixed_t value)
{
    struct vo_wayland_state *wl = data;

    // value is 10.00 on a normal mouse wheel
    // scale it down to 1.00 for multipliying it with the commands
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        if (value > 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_UP,
                    wl_fixed_to_double(value)*0.1);
        if (value < 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_DOWN,
                    wl_fixed_to_double(value)*-0.1);
    }
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        if (value > 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_LEFT,
                    wl_fixed_to_double(value)*0.1);
        if (value < 0)
            mp_input_put_axis(wl->vo->input_ctx, MP_AXIS_RIGHT,
                    wl_fixed_to_double(value)*-0.1);
    }
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

static void seat_handle_capabilities(void *data,
                                     struct wl_seat *seat,
                                     enum wl_seat_capability caps)
{
    struct vo_wayland_state *wl = data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->input.keyboard) {
        wl->input.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_set_user_data(wl->input.keyboard, wl);
        wl_keyboard_add_listener(wl->input.keyboard, &keyboard_listener, wl);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->input.keyboard) {
        wl_keyboard_destroy(wl->input.keyboard);
        wl->input.keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->input.pointer) {
        wl->input.pointer = wl_seat_get_pointer(seat);
        wl_pointer_set_user_data(wl->input.pointer, wl);
        wl_pointer_add_listener(wl->input.pointer, &pointer_listener, wl);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->input.pointer) {
        wl_pointer_destroy(wl->input.pointer);
        wl->input.pointer = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
};

/* SHM LISTENER */
static void shm_handle_format(void *data,
                              struct wl_shm *wl_shm,
                              uint32_t format)
{
    struct vo_wayland_state *wl = data;
    wl->display.formats |= (1 << format);
}

const struct wl_shm_listener shm_listener = {
    shm_handle_format
};

static void registry_handle_global (void *data,
                                    struct wl_registry *registry,
                                    uint32_t id,
                                    const char *interface,
                                    uint32_t version)
{
    struct vo_wayland_state *wl = data;
    struct wl_registry *reg = wl->display.registry;

    if (strcmp(interface, "wl_compositor") == 0) {

        wl->display.compositor = wl_registry_bind(reg, id,
                                                  &wl_compositor_interface, 1);
    }

    else if (strcmp(interface, "wl_shell") == 0) {

        wl->display.shell = wl_registry_bind(reg, id, &wl_shell_interface, 1);
    }

    else if (strcmp(interface, "wl_shm") == 0) {

        wl->cursor.shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
        wl->cursor.theme = wl_cursor_theme_load(NULL, 32, wl->cursor.shm);
        wl->cursor.default_cursor = wl_cursor_theme_get_cursor(wl->cursor.theme,
                                                              "left_ptr");
        wl_shm_add_listener(wl->cursor.shm, &shm_listener, wl);
    }

    else if (strcmp(interface, "wl_output") == 0) {

        struct vo_wayland_output *output =
            talloc_zero(wl, struct vo_wayland_output);

        output->id = id;
        output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&wl->display.output_list, &output->link);
    }

    else if (strcmp(interface, "wl_seat") == 0) {

        wl->input.seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);

        wl_seat_add_listener(wl->input.seat, &seat_listener, wl);
    }
}

static void registry_handle_global_remove (void *data,
                                           struct wl_registry *registry,
                                           uint32_t id)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};


/*** internal functions ***/

static int lookupkey(int key)
{
    static const char *passthrough_keys
        = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";

    int mpkey = 0;
    if ((key >= 'a' && key <= 'z') ||
        (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        mpkey = key;

    if (!mpkey)
        mpkey = lookup_keymap_table(keymap, key);

    return mpkey;
}

static void hide_cursor (struct vo_wayland_state *wl)
{
    if (!wl->cursor.pointer)
        return;

    wl_pointer_set_cursor(wl->cursor.pointer, wl->cursor.serial, NULL, 0, 0);
}

static void show_cursor (struct vo_wayland_state *wl)
{
    if (!wl->cursor.pointer)
        return;

    struct wl_cursor_image *image  = wl->cursor.default_cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);

    wl_pointer_set_cursor(wl->cursor.pointer,
                          wl->cursor.serial,
                          wl->cursor.surface,
                          image->hotspot_x,
                          image->hotspot_y);

    wl_surface_attach(wl->cursor.surface, buffer, 0, 0);
    wl_surface_damage(wl->cursor.surface, 0, 0, image->width, image->height);
    wl_surface_commit(wl->cursor.surface);
}

static void resize_window(struct vo_wayland_state *wl,
                          uint32_t edges,
                          int32_t width,
                          int32_t height)
{
    if (wl->window.resize_func && wl->window.resize_func_data) {
        wl->window.resize_func(wl, edges, width, height,
                               wl->window.resize_func_data);
        wl->window.events |= VO_EVENT_RESIZE;
    }
    else
        mp_msg(MSGT_VO, MSGL_WARN, "[wayland] No resizing possible!\n");
}


static bool create_display (struct vo_wayland_state *wl)
{
    wl->display.display = wl_display_connect(NULL);

    if (!wl->display.display)
        return false;

    wl_list_init(&wl->display.output_list);
    wl->display.registry = wl_display_get_registry(wl->display.display);
    wl_registry_add_listener(wl->display.registry, &registry_listener, wl);

    wl_display_dispatch(wl->display.display);

    wl->display.display_fd = wl_display_get_fd(wl->display.display);

    return true;
}

static void destroy_display (struct vo_wayland_state *wl)
{
    if (wl->display.shell)
        wl_shell_destroy(wl->display.shell);

    if (wl->display.compositor)
        wl_compositor_destroy(wl->display.compositor);

    wl_registry_destroy(wl->display.registry);
    wl_display_flush(wl->display.display);
    wl_display_disconnect(wl->display.display);
}

static bool create_window (struct vo_wayland_state *wl)
{
    wl->window.surface = wl_compositor_create_surface(wl->display.compositor);
    wl->window.shell_surface = wl_shell_get_shell_surface(wl->display.shell,
                                                          wl->window.surface);

    if (!wl->window.shell_surface)
        return false;

    wl_shell_surface_add_listener(wl->window.shell_surface,
                                  &shell_surface_listener, wl);

    wl_shell_surface_set_toplevel(wl->window.shell_surface);
    wl_shell_surface_set_class(wl->window.shell_surface, "mpv");

    return true;
}

static void destroy_window (struct vo_wayland_state *wl)
{
    wl_shell_surface_destroy(wl->window.shell_surface);
    wl_surface_destroy(wl->window.surface);
}

static bool create_cursor (struct vo_wayland_state *wl)
{
    wl->cursor.surface =
        wl_compositor_create_surface(wl->display.compositor);

    return wl->cursor.surface != NULL;
}

static void destroy_cursor (struct vo_wayland_state *wl)
{
    if (wl->cursor.theme)
        wl_cursor_theme_destroy(wl->cursor.theme);

    if (wl->cursor.surface)
        wl_surface_destroy(wl->cursor.surface);
}

static bool create_input (struct vo_wayland_state *wl)
{
    wl->input.xkb.context = xkb_context_new(0);

    if (!wl->input.xkb.context) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wayland] failed to initialize input.\n");
        return false;
    }

    return true;
}


static void destroy_input (struct vo_wayland_state *wl)
{
    if (wl->input.keyboard)
        wl_keyboard_destroy(wl->input.keyboard);

    if (wl->input.pointer)
        wl_pointer_destroy(wl->input.pointer);

    if (wl->input.seat)
        wl_seat_destroy(wl->input.seat);

    xkb_context_unref(wl->input.xkb.context);
}

/*** mplayer2 interface ***/

int vo_wayland_init (struct vo *vo)
{
    vo->wayland = talloc_zero(NULL, struct vo_wayland_state);
    struct vo_wayland_state *wl = vo->wayland;
    wl->vo = vo;

    if (!create_input(wl)
        || !create_display(wl)
        || !create_window(wl)
        || !create_cursor(wl))
    {
        mp_msg(MSGT_VO, MSGL_ERR, "[wayland] failed to initialize display.\n");
        return false;
    }

    vo->event_fd = wl->display.display_fd;

    return true;
}

void vo_wayland_uninit (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    destroy_cursor(wl);
    destroy_window(wl);
    destroy_display(wl);
    destroy_input(wl);
    talloc_free(wl);
    vo->wayland = NULL;
}

static void vo_wayland_ontop (struct vo *vo)
{
    vo->opts->ontop = !vo->opts->ontop;
    vo->opts->fullscreen = !vo->opts->fullscreen;

    /* use the already existing code to leave fullscreen mode and go into
     * toplevel mode */
    vo_wayland_fullscreen(vo);
}

static void vo_wayland_border (struct vo *vo)
{
    /* wayland clienst have to do the decorations themself
     * (client side decorations) but there is no such code implement nor
     * do I plan on implementing something like client side decorations
     *
     * The only exception would be resizing on when clicking and dragging
     * on the border region of the window but this should be discussed at first
     */
}

static void vo_wayland_fullscreen (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    if (!wl->display.shell)
        return;

    struct wl_output *fs_output = wl->display.fs_output;

    if (vo->opts->fullscreen) {
        wl->window.p_width = wl->window.width;
        wl->window.p_height = wl->window.height;
        wl_shell_surface_set_fullscreen(wl->window.shell_surface,
                WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
                0, fs_output);
    }

    else {
        wl_shell_surface_set_toplevel(wl->window.shell_surface);
        resize_window(wl, 0, wl->window.p_width, wl->window.p_height);
    }
}

static int vo_wayland_check_events (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    struct wl_display *dp = wl->display.display;
    int ret;

    wl_display_dispatch_pending(dp);
    wl_display_flush(dp);

    struct pollfd fd = {
        wl->display.display_fd,
        POLLIN | POLLOUT | POLLERR | POLLHUP,
        0
    };

    /* wl_display_dispatch is blocking
     * wl_dipslay_dispatch_pending is non-blocking but does not read from the fd
     *
     * when pausing no input events get queued so we have to check if there
     * are events to read from the file descriptor through poll */
    if (poll(&fd, 1, 0) > 0) {
        if (fd.revents & POLLERR || fd.revents & POLLHUP)
            mp_msg(MSGT_VO, MSGL_ERR, "[wayland] error occurred on fd\n");
        if (fd.revents & POLLIN)
            wl_display_dispatch(dp);
        if (fd.revents & POLLOUT)
            wl_display_flush(dp);
    }

    ret = wl->window.events;
    wl->window.events = 0;

    return ret;
}

static void vo_wayland_update_screeninfo (struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wayland;
    struct mp_vo_opts *opts = vo->opts;
    bool mode_received = false;

    wl_display_roundtrip(wl->display.display);

    vo->xinerama_x = vo->xinerama_y = 0;

    int screen_id = 0;

    struct vo_wayland_output *output;
    struct vo_wayland_output *first_output = NULL;
    struct vo_wayland_output *fsscreen_output = NULL;

    wl_list_for_each_reverse(output, &wl->display.output_list, link) {
        if (!output || !output->width)
            continue;

        mode_received = true;

        if (opts->fsscreen_id == screen_id)
            fsscreen_output = output;

        if (!first_output)
            first_output = output;

        screen_id++;
    }

    if (!mode_received) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wayland] no output mode detected\n");
        return;
    }

    if (fsscreen_output) {
        wl->display.fs_output = fsscreen_output->output;
        opts->screenwidth = fsscreen_output->width;
        opts->screenheight = fsscreen_output->height;
    }
    else {
        wl->display.fs_output = NULL; /* current output is always 0 */

        if (first_output) {
            opts->screenwidth = first_output->width;
            opts->screenheight = first_output->height;
        }
    }

    aspect_save_screenres(vo, opts->screenwidth, opts->screenheight);
}

int vo_wayland_control (struct vo *vo, int *events, int request, void *arg)
{
    struct vo_wayland_state *wl = vo->wayland;
    wl_display_dispatch_pending(wl->display.display);

    switch (request) {
    case VOCTRL_CHECK_EVENTS:
        *events |= vo_wayland_check_events(vo);
        return VO_TRUE;
    case VOCTRL_FULLSCREEN:
        vo_wayland_fullscreen(vo);
        *events |= VO_EVENT_RESIZE;
        return VO_TRUE;
    case VOCTRL_ONTOP:
        vo_wayland_ontop(vo);
        return VO_TRUE;
    case VOCTRL_BORDER:
        vo_wayland_border(vo);
        *events |= VO_EVENT_RESIZE;
        return VO_TRUE;
    case VOCTRL_UPDATE_SCREENINFO:
        vo_wayland_update_screeninfo(vo);
        return VO_TRUE;
    case VOCTRL_SET_CURSOR_VISIBILITY:
        if (*(bool *)arg) {
            if (!wl->cursor.visible)
                show_cursor(wl);
        }
        else {
            if (wl->cursor.visible)
                hide_cursor(wl);
        }
        wl->cursor.visible = *(bool *)arg;
        return VO_TRUE;
    case VOCTRL_UPDATE_WINDOW_TITLE:
        wl_shell_surface_set_title(wl->window.shell_surface, (char *) arg);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

bool vo_wayland_config (struct vo *vo, uint32_t d_width,
                        uint32_t d_height, uint32_t flags)
{
    struct vo_wayland_state *wl = vo->wayland;

    wl->window.width = d_width;
    wl->window.height = d_height;
    wl->window.p_width = d_width;
    wl->window.p_height = d_height;
    wl->window.aspect = wl->window.width / (float) MPMAX(wl->window.height, 1);

    vo_wayland_fullscreen(vo);

    return true;
}

#include "labwc.h"

bool is_toplevel(struct view *view)
{
	switch (view->type) {
	case LAB_XDG_SHELL_VIEW:
		return view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL;
	case LAB_XWAYLAND_VIEW:
		return xwl_nr_parents(view) > 0 ? false : true;
	}
	return false;
}

struct view *first_toplevel(struct server *server)
{
	struct view *view;

	wl_list_for_each (view, &server->views, link) {
		if (!view->been_mapped) {
			continue;
		}
		if (is_toplevel(view)) {
			return view;
		}
	}
	fprintf(stderr, "warn: found no toplevel view (%s)\n", __func__);
	return NULL;
}

static struct view *last_toplevel(struct server *server)
{
	struct view *view;

	wl_list_for_each_reverse (view, &server->views, link) {
		if (!view->been_mapped)
			continue;
		if (is_toplevel(view))
			return view;
	}
	/* no top-level view */
	return NULL;
}

void view_focus_last_toplevel(struct server *server)
{
	/* TODO: write view_nr_toplevel_views() */
	if (wl_list_length(&server->views) < 2)
		return;
	struct view *view = last_toplevel(server);
	view_focus(view);
}

static struct view *next_toplevel(struct view *current)
{
	struct view *tmp = current;

	do {
		tmp = wl_container_of(tmp->link.next, tmp, link);
	} while (!tmp->been_mapped || !is_toplevel(tmp));
	return tmp;
}

/* Original function name - suggest we keep it */
void view_focus_next_toplevel(struct view *current)
{
	struct view *view;
	view = next_toplevel(current);
	view_focus(view);
}

static void move_to_front(struct view *view)
{
	wl_list_remove(&view->link);
	wl_list_insert(&view->server->views, &view->link);
}

static void activate_view(struct view *view)
{
	if (view->type == LAB_XDG_SHELL_VIEW) {
		wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
	} else if (view->type == LAB_XWAYLAND_VIEW) {
		wlr_xwayland_surface_activate(view->xwayland_surface, true);
	} else {
		fprintf(stderr, "warn: view was of unknown type (%s)\n",
			__func__);
	}
}

/**
 * Request that this toplevel surface show itself in an activated or
 * deactivated state.
 */
static void set_activated(struct wlr_surface *s, bool activated)
{
	if (!s)
		return;
	if (wlr_surface_is_xdg_surface(s)) {
		struct wlr_xdg_surface *previous;
		previous = wlr_xdg_surface_from_wlr_surface(s);
		wlr_xdg_toplevel_set_activated(previous, activated);
	} else {
		struct wlr_xwayland_surface *previous;
		previous = wlr_xwayland_surface_from_wlr_surface(s);
		wlr_xwayland_surface_activate(previous, activated);
	}
}

void view_focus(struct view *view)
{
	/* Note: this function only deals with keyboard focus. */
	if (!view || !view->surface)
		return;
	struct server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface;

	prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == view->surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (view->type == LAB_XWAYLAND_VIEW) {
		/* Don't focus on menus, etc */
		move_to_front(view);
		if (!wlr_xwayland_or_surface_wants_focus(
			    view->xwayland_surface)) {
			return;
		}
	}
	if (prev_surface) {
		set_activated(seat->keyboard_state.focused_surface, false);
	}

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	move_to_front(view);
	activate_view(view);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will
	 * keep track of this and automatically send key events to the
	 * appropriate clients without additional work on your part.
	 */
	wlr_seat_keyboard_notify_enter(seat, view->surface, keyboard->keycodes,
				       keyboard->num_keycodes,
				       &keyboard->modifiers);
}

bool view_want_deco(struct view *view)
{
	if (view->type != LAB_XWAYLAND_VIEW)
		return false;
	if (!is_toplevel(view))
		return false;
	if (view->xwayland_surface->override_redirect)
		return false;
	if (view->xwayland_surface->decorations !=
	    WLR_XWAYLAND_SURFACE_DECORATIONS_ALL)
		return false;
	return true;
}

void begin_interactive(struct view *view, enum cursor_mode mode, uint32_t edges)
{
	/* This function sets up an interactive move or resize operation, where
	 * the compositor stops propegating pointer events to clients and
	 * instead consumes them itself, to move or resize windows. */
	struct server *server = view->server;
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo_box;
		switch (view->type) {
		case LAB_XDG_SHELL_VIEW:
			wlr_xdg_surface_get_geometry(view->xdg_surface,
						     &geo_box);
			break;
		case LAB_XWAYLAND_VIEW:
			geo_box.x = view->xwayland_surface->x;
			geo_box.y = view->xwayland_surface->y;
			geo_box.width = view->xwayland_surface->width;
			geo_box.height = view->xwayland_surface->height;
			break;
		}

		double border_x =
			(view->x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y =
			(view->y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->grab_box = geo_box;
		server->grab_box.x += view->x;
		server->grab_box.y += view->y;
		server->resize_edges = edges;
	}
}

static bool _view_at(struct view *view, double lx, double ly,
		     struct wlr_surface **surface, double *sx, double *sy)
{
	/*
	 * XDG toplevels may have nested surfaces, such as popup windows for
	 * context menus or tooltips. This function tests if any of those are
	 * underneath the coordinates lx and ly (in output Layout Coordinates).
	 * If so, it sets the surface pointer to that wlr_surface and the sx and
	 * sy coordinates to the coordinates relative to that surface's top-left
	 * corner.
	 */
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;
	double _sx, _sy;
	struct wlr_surface *_surface = NULL;

	switch (view->type) {
	case LAB_XDG_SHELL_VIEW:
		_surface = wlr_xdg_surface_surface_at(
			view->xdg_surface, view_sx, view_sy, &_sx, &_sy);
		break;
	case LAB_XWAYLAND_VIEW:
		if (!view->xwayland_surface->surface)
			return false;
		_surface =
			wlr_surface_surface_at(view->xwayland_surface->surface,
					       view_sx, view_sy, &_sx, &_sy);
		break;
	}

	if (_surface) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}
	return false;
}

struct view *view_at(struct server *server, double lx, double ly,
		     struct wlr_surface **surface, double *sx, double *sy,
		     int *view_area)
{
	/*
	 * This iterates over all of our surfaces and attempts to find one under
	 * the cursor. It relies on server->views being ordered from
	 * top-to-bottom.
	 */
	struct view *view;
	wl_list_for_each (view, &server->views, link) {
		if (_view_at(view, lx, ly, surface, sx, sy))
			return view;
		if (!view_want_deco(view))
			continue;
		if (deco_at(view, lx, ly) == LAB_DECO_PART_TOP) {
			*view_area = LAB_DECO_PART_TOP;
			return view;
		}
	}
	return NULL;
}
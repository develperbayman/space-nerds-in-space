#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#define DEFINE_UI_ELEMENT_GLOBALS
#include "snis_ui_element.h"

struct ui_element {
	void *element;
	int active_displaymode;
	volatile int *displaymode;
	ui_element_drawing_function draw;
	ui_element_button_release_function button_release;
	ui_element_set_focus_function set_focus;
	int has_focus;
	ui_element_keypress_function keypress_fn, keyrelease_fn;
	ui_element_inside_function inside_fn;
	ui_update_mouse_pos_function update_mouse_pos;
	int hidden;
	char *tooltip;
	int tooltip_timer;
};

struct ui_element_list {
	struct ui_element *element;
	struct ui_element_list *next;
};

static void (*draw_tooltip)(int mousex, int mousey, char *tooltip);

struct ui_element *ui_element_init(void *element,
			ui_element_drawing_function draw,
			ui_element_button_release_function button_release,
			ui_element_inside_function inside_fn,
			int active_displaymode, volatile int *displaymode)
{
	struct ui_element *e;

	e = malloc(sizeof(*e));
	e->element = element;
	e->draw = draw;
	e->button_release = button_release;
	e->inside_fn = inside_fn;
	e->active_displaymode = active_displaymode;
	e->displaymode = displaymode;
	e->set_focus = NULL;
	e->has_focus = 0;
	e->keypress_fn = NULL;	
	e->keyrelease_fn = NULL;
	e->hidden = 0;
	e->tooltip = NULL;
	e->tooltip_timer = TOOLTIP_DELAY;
	e->update_mouse_pos = NULL;
	return e;
}

void ui_element_draw(struct ui_element *element)
{
	element->draw(element);
}

void ui_element_maybe_draw_tooltip(struct ui_element *element, int mousex, int mousey)
{
	if (element->update_mouse_pos)
		element->update_mouse_pos(element->element, mousex, mousey);
	if (!element->inside_fn) {
		return;
	}
	if (!element->tooltip)
		return;
	if (!element->inside_fn(element->element, mousex, mousey)) {
		element->tooltip_timer = TOOLTIP_DELAY;
		return;
	}
	if (element->tooltip_timer > 0)
		element->tooltip_timer--;
	if (element->tooltip_timer > 0)
		return;
	if (draw_tooltip)
		draw_tooltip(mousex, mousey, element->tooltip);
}

void ui_element_list_add_element(struct ui_element_list **list,
					struct ui_element *element)
{
	struct ui_element_list *l, *i;

	if (*list == NULL) {
		*list = malloc(sizeof(**list));
		(*list)->element = element;
		(*list)->next = NULL;
		return;
	}
	l = malloc(sizeof(*l));
	l->element = element;
	l->next = NULL;
	/* We need to add it to the end of the list, not the beginning
	 * so that the focus order is correct.
	 */
	for (i = *list; i->next != NULL;)
		i = i->next;
	i->next = l;
}

void ui_element_list_free(struct ui_element_list *list)
{
	struct ui_element_list *n;

	if (list == NULL)
		return;
	n = list->next;
	free(list->element);
	free(list);
	ui_element_list_free(n);
}

void ui_element_list_draw(struct ui_element_list *list)
{
	for (; list != NULL; list = list->next) {
		struct ui_element *e = list->element;
		if (e->draw && e->active_displaymode == *e->displaymode && !e->hidden)
			e->draw(e->element);
	}
}

void ui_element_list_maybe_draw_tooltips(struct ui_element_list *list, int mousex, int mousey)
{
	for (; list != NULL; list = list->next) {
		struct ui_element *e = list->element;
		if (e->draw && e->active_displaymode == *e->displaymode && !e->hidden)
			ui_element_maybe_draw_tooltip(e, mousex, mousey);
	}
}

void ui_element_set_tooltip(struct ui_element *element, char *tooltip)
{
	if (element->tooltip) {
		free(element->tooltip);
		element->tooltip = NULL;
	}
	if (!tooltip)
		return;
	if (strcmp(tooltip, "") == 0)
		return;
	element->tooltip = strdup(tooltip);
}

void ui_set_focus(struct ui_element_list *list, struct ui_element *e, int has_focus)
{
	if (!e->set_focus)
		return;

	for (; list != NULL; list = list->next) {
		if (!list->element->set_focus)
			continue;
		if (list->element == e) {
			e->set_focus(e->element, has_focus);
			e->has_focus = has_focus;
		} else {
			list->element->set_focus(list->element->element, 0);
			list->element->has_focus = 0;
		}
	}
}

void ui_element_list_button_release(struct ui_element_list *list, int x, int y)
{
	int hit;
	struct ui_element *e;
	struct ui_element_list *i;

	for (i = list; i != NULL; i = i->next) {
		e = i->element;
		if (e->button_release && e->active_displaymode == *e->displaymode && !e->hidden) {
			/* If we have the inside_fn, use it so that we can set the focus before
			 * triggering the button action in case the button action wants to set the
			 * focus, otherwise, if we set the focus afterwards, it will undo the
			 * button action's focus setting.
			 */
			if (e->inside_fn) {
				hit = e->inside_fn(e->element, x, y);
				if (hit) {
					ui_set_focus(list, e, 1);
					(void) e->button_release(e->element, x, y);
					break;
				}
			} else {
				hit = e->button_release(e->element, x, y);
				if (hit) {
					ui_set_focus(list, e, 1);
					break;
				}
			}
		}
	}
}

void ui_element_set_focus_callback(struct ui_element *e,
					ui_element_set_focus_function set_focus)
{
	e->set_focus = set_focus;
}

void ui_element_set_inside_callback(struct ui_element *e,
					ui_element_inside_function inside_fn)
{
	e->inside_fn = inside_fn;
}

void ui_element_set_displaymode(struct ui_element *e, int displaymode)
{
	e->active_displaymode = displaymode;
}

static void advance_focus(struct ui_element_list *list)
{
	struct ui_element_list *i;
	int found = 0;
	int done = 0;

	for (i = list; i; i = i->next) {
		struct ui_element *e = i->element;
		if (e->set_focus && e->has_focus &&
				e->active_displaymode == *e->displaymode) {
			e->has_focus = 0;
			e->set_focus(e->element, 0);
			found = 1;
			continue;
		}
		if (found) {
			if (e->active_displaymode == *e->displaymode &&
				e->set_focus && !e->hidden) {
				e->has_focus = 1;
				e->set_focus(e->element, 1);
				done = 1;
				break;				
			}
		}
		
	}

	if (done)
		return;

	/* Start from the beginning of the list */
	for (i = list; i; i = i->next) {
		struct ui_element *e = i->element;
		if (e->active_displaymode == *e->displaymode && e->set_focus && !e->hidden) {
			e->has_focus = 1;
			e->set_focus(e->element, 1);
			break;
		}
	}
}

void ui_element_list_keypress(struct ui_element_list *list, GdkEventKey *event)
{
	struct ui_element_list *i;

	if (event->type == GDK_KEY_PRESS && (event->keyval & ~0x7f) != 0) {
		switch (event->keyval) {
		case GDK_KEY_Tab:
			advance_focus(list);
			return;
		default:
			break;
		}
	}

	for (i = list; i; i = i->next) {
		if (!i->element->has_focus)
			continue;
		if (!i->element->keypress_fn)
			continue;
		if (*i->element->displaymode != i->element->active_displaymode)
			continue;
		if (i->element->hidden)
			continue;
		i->element->keypress_fn(i->element->element, event);
		break;
	}
}

void ui_element_list_keyrelease(struct ui_element_list *list, GdkEventKey *event)
{
	struct ui_element_list *i;

	for (i = list; i; i = i->next) {
		if (!i->element->has_focus)
			continue;
		if (!i->element->keyrelease_fn)
			continue;
		if (*i->element->displaymode != i->element->active_displaymode)
			continue;
		if (!i->element->hidden)
			continue;
		i->element->keyrelease_fn(i->element->element, event);
		break;
	}
}

void ui_element_get_keystrokes(struct ui_element *e,
				ui_element_keypress_function keypress_fn,
				ui_element_keypress_function keyrelease_fn)
{
	e->keypress_fn = keypress_fn;
	e->keyrelease_fn = keyrelease_fn;
}

void ui_element_hide(struct ui_element *e)
{
	e->hidden = 1;
}

void ui_element_unhide(struct ui_element *e)
{
	e->hidden = 0;
}

struct ui_element *widget_to_ui_element(struct ui_element_list *list, void *widget)
{
	for (; list != NULL; list = list->next) {
		if (list->element->element == widget)
			return list->element;
	}
	return NULL;
}

void ui_set_widget_focus(struct ui_element_list *list, void *widget)
{
	struct ui_element *uie = widget_to_ui_element(list, widget);
	if (uie)
		ui_set_focus(list, uie, 1);
}

void ui_set_tooltip_drawing_function(ui_tooltip_drawing_function f)
{
	draw_tooltip = f;
}

void ui_set_update_mouse_pos_callback(struct ui_element *e, ui_update_mouse_pos_function f)
{
	e->update_mouse_pos = f;
}

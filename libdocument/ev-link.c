/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2005 Red Hat, Inc.
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include "ev-link.h"

enum {
	PROP_0,
	PROP_TITLE,
	PROP_ACTION
};

struct _EvLink {
	GObject base_instance;
	EvLinkPrivate *priv;
};

struct _EvLinkClass {
	GObjectClass base_class;
};

struct _EvLinkPrivate {
	gchar        *title;
	EvLinkAction *action;
};

G_DEFINE_TYPE (EvLink, ev_link, G_TYPE_OBJECT)

#define EV_LINK_GET_PRIVATE(object) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((object), EV_TYPE_LINK, EvLinkPrivate))

const gchar *
ev_link_get_title (EvLink *self)
{
	g_return_val_if_fail (EV_IS_LINK (self), NULL);
	
	return self->priv->title;
}

EvLinkAction *
ev_link_get_action (EvLink *self)
{
	g_return_val_if_fail (EV_IS_LINK (self), NULL);
	
	return self->priv->action;
}

static void
ev_link_get_property (GObject    *object,
		      guint       prop_id,
		      GValue     *value,
		      GParamSpec *param_spec)
{
	EvLink *self;

	self = EV_LINK (object);

	switch (prop_id) {
	        case PROP_TITLE:
			g_value_set_string (value, self->priv->title);
			break;
	        case PROP_ACTION:
			g_value_set_pointer (value, self->priv->action);
			break;
	        default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object,
							   prop_id,
							   param_spec);
			break;
	}
}

static void
ev_link_set_property (GObject      *object,
		      guint         prop_id,
		      const GValue *value,
		      GParamSpec   *param_spec)
{
	EvLink *self = EV_LINK (object);
	
	switch (prop_id) {
	        case PROP_TITLE:
			self->priv->title = g_value_dup_string (value);	
			break;
	        case PROP_ACTION:
			self->priv->action = g_value_get_pointer (value);
			break;
	        default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object,
							   prop_id,
							   param_spec);
			break;
	}
}

static void
ev_link_finalize (GObject *object)
{
	EvLinkPrivate *priv;

	priv = EV_LINK (object)->priv;

	if (priv->title) {
		g_free (priv->title);
		priv->title = NULL;
	}

	if (priv->action) {
		g_object_unref (priv->action);
		priv->action = NULL;
	}

	G_OBJECT_CLASS (ev_link_parent_class)->finalize (object);
}

static void
ev_link_init (EvLink *ev_link)
{
	ev_link->priv = EV_LINK_GET_PRIVATE (ev_link);

	ev_link->priv->title = NULL;
	ev_link->priv->action = NULL;
}

static void
ev_link_class_init (EvLinkClass *ev_window_class)
{
	GObjectClass *g_object_class;

	g_object_class = G_OBJECT_CLASS (ev_window_class);

	g_object_class->set_property = ev_link_set_property;
	g_object_class->get_property = ev_link_get_property;

	g_object_class->finalize = ev_link_finalize;

	g_type_class_add_private (g_object_class, sizeof (EvLinkPrivate));

	g_object_class_install_property (g_object_class,
					 PROP_TITLE,
					 g_param_spec_string ("title",
				     	 		      "Link Title",
				     			      "The link title",
							      NULL,
							      G_PARAM_READWRITE |
				     			      G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (g_object_class,
					 PROP_ACTION,
					 g_param_spec_pointer ("action",
							       "Link Action",
							       "The link action",
							       G_PARAM_READWRITE |
							       G_PARAM_CONSTRUCT_ONLY));
}

EvLink *
ev_link_new (const char   *title,
	     EvLinkAction *action)
{
	return EV_LINK (g_object_new (EV_TYPE_LINK,
				      "title", title,
				      "action", action,
				      NULL));
}

/* Link Mapping stuff */
static void
ev_link_mapping_free_foreach (EvLinkMapping *mapping)
{
	g_object_unref (G_OBJECT (mapping->link));
	g_free (mapping);
}

void
ev_link_mapping_free (GList *link_mapping)
{
	if (link_mapping == NULL)
		return;

	g_list_foreach (link_mapping, (GFunc) (ev_link_mapping_free_foreach), NULL);
	g_list_free (link_mapping);
}

EvLink *
ev_link_mapping_find (GList   *link_mapping,
		      gdouble  x,
		      gdouble  y)
{
	GList *list;
	EvLink *link = NULL;
	int i;
	
	i = 0;

	for (list = link_mapping; list; list = list->next) {
		EvLinkMapping *mapping = list->data;

		i++;
		if ((x >= mapping->x1) &&
		    (y >= mapping->y1) &&
		    (x <= mapping->x2) &&
		    (y <= mapping->y2)) {
			link = mapping->link;
			break;
		}
	}

	return link;
}

gint
ev_link_get_page (EvLink *link)
{
	EvLinkAction *action;
	EvLinkDest *dest;

	action = ev_link_get_action (link);
	if (!action)
		return -1;

	if (ev_link_action_get_action_type (action) !=
	    EV_LINK_ACTION_TYPE_GOTO_DEST)
		return -1;

	dest = ev_link_action_get_dest (action);
	if (dest)
		return ev_link_dest_get_page (dest);
		
	return -1;
}

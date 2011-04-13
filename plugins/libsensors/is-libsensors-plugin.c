/*
 * Copyright (C) 2011 Alex Murray <murray.alex@gmail.com>
 *
 * indicator-sensors is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * indicator-sensors is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with indicator-sensors.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "is-libsensors-plugin.h"
#include <stdlib.h>
#include <is-temperature-sensor.h>
#include <is-indicator.h>
#include <sensors/sensors.h>
#include <sensors/error.h>
#include <glib/gi18n.h>

static void peas_activatable_iface_init(PeasActivatableInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(IsLibsensorsPlugin,
			       is_libsensors_plugin,
			       PEAS_TYPE_EXTENSION_BASE,
			       0,
			       G_IMPLEMENT_INTERFACE_DYNAMIC(PEAS_TYPE_ACTIVATABLE,
							     peas_activatable_iface_init));

enum {
	PROP_OBJECT = 1,
};

struct _IsLibsensorsPluginPrivate
{
	IsIndicator *indicator;
	gboolean inited;
	GHashTable *sensor_chip_names;
};

static void is_libsensors_plugin_finalize(GObject *object);

static void
is_libsensors_plugin_set_property(GObject *object,
					 guint prop_id,
					 const GValue *value,
					 GParamSpec *pspec)
{
	IsLibsensorsPlugin *plugin = IS_LIBSENSORS_PLUGIN(object);

	switch (prop_id) {
	case PROP_OBJECT:
		plugin->priv->indicator = IS_INDICATOR(g_value_dup_object(value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
is_libsensors_plugin_get_property(GObject *object,
					 guint prop_id,
					 GValue *value,
					 GParamSpec *pspec)
{
	IsLibsensorsPlugin *plugin = IS_LIBSENSORS_PLUGIN(object);

	switch (prop_id) {
	case PROP_OBJECT:
		g_value_set_object(value, plugin->priv->indicator);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
is_libsensors_plugin_init(IsLibsensorsPlugin *self)
{
	IsLibsensorsPluginPrivate *priv =
		G_TYPE_INSTANCE_GET_PRIVATE(self, IS_TYPE_LIBSENSORS_PLUGIN,
					    IsLibsensorsPluginPrivate);

	self->priv = priv;
        if (sensors_init(NULL) == 0) {
		priv->sensor_chip_names = g_hash_table_new_full(g_str_hash,
								g_str_equal,
								g_free,
								NULL);
		priv->inited = TRUE;
	}
}

static void
is_libsensors_plugin_finalize(GObject *object)
{
	IsLibsensorsPlugin *self = (IsLibsensorsPlugin *)object;
	IsLibsensorsPluginPrivate *priv = self->priv;

	if (priv->sensor_chip_names) {
		g_hash_table_destroy(priv->sensor_chip_names);
	}
	/* think about storing this in the class structure so we only init once
	   and unload once */
	if (priv->inited) {
		sensors_cleanup();
		priv->inited = FALSE;
	}
	G_OBJECT_CLASS(is_libsensors_plugin_parent_class)->finalize(object);
}

static gchar *get_chip_name_string(const sensors_chip_name *chip) {
	gchar *name = NULL;

	// adapted from lm-sensors:prog/sensors/main.c:sprintf_chip_name
        // in lm-sensors-3.0
#define BUF_SIZE 200
	name = g_malloc0(BUF_SIZE);
	if (sensors_snprintf_chip_name(name, BUF_SIZE, chip) < 0) {
		g_free(name);
                name = NULL;
        }
#undef BUF_SIZE
	return name;
}

static void
update_sensor_value(IsSensor *sensor,
		    IsLibsensorsPlugin *self)
{
	const gchar *id;
	const sensors_chip_name *found_chip;
	gchar *offset, *end;
	int n, ret;
	gdouble value = 0.0f;

	id = is_sensor_get_id(sensor);

	found_chip = g_hash_table_lookup(self->priv->sensor_chip_names,
					 id);
	g_assert(found_chip != NULL);

	offset = g_strrstr(id, "/");
	/* we make these string so should always have a / */
	g_assert(offset != NULL);

	n = (int)g_ascii_strtoll(offset + 1, &end, 10);
	/* conversion should also never fail */
	g_assert(end != NULL);

	if ((ret = sensors_get_value(found_chip, n, &value)) < 0) {
		GError *error = g_error_new(g_quark_from_string("libsensors-plugin-error-quark"),
					    0,
					    /* first placeholder is sensor name,
					     * second is error message */
					    _("Error getting sensor value for sensor %s: %s"),
					    id, sensors_strerror(ret));
		is_sensor_emit_error(sensor, error);
		g_error_free(error);
		goto out;
	}
	is_sensor_set_value(sensor, value);

out:
	return;
}

static void
process_sensors_chip_name(IsLibsensorsPlugin *self,
			  const sensors_chip_name *chip_name)
{
	IsLibsensorsPluginPrivate *priv = self->priv;
	gchar *chip_name_string = NULL;
	gchar *label = NULL;
	const sensors_subfeature *input_feature;
	const sensors_subfeature *low_feature;
	const sensors_subfeature *high_feature;
	const sensors_feature *main_feature;
	gint nr1 = 0;
	gdouble value, low, high;
	gchar *id;
	IsSensor *sensor;

	chip_name_string = get_chip_name_string(chip_name);
	if (chip_name_string == NULL) {
		g_warning("libsensors plugin: error getting name string for sensor '%s'",
			  chip_name->path);
		goto out;
	}
	while ((main_feature = sensors_get_features(chip_name, &nr1)))
	{
		switch (main_feature->type)
		{
		case SENSORS_FEATURE_IN:
			input_feature = sensors_get_subfeature(chip_name,
							       main_feature,
							       SENSORS_SUBFEATURE_IN_INPUT);
			low_feature = sensors_get_subfeature(chip_name,
							     main_feature,
							     SENSORS_SUBFEATURE_IN_MIN);
			high_feature = sensors_get_subfeature(chip_name,
							      main_feature,
							      SENSORS_SUBFEATURE_IN_MAX);
			break;
		case SENSORS_FEATURE_FAN:
			input_feature = sensors_get_subfeature(chip_name,
							       main_feature,
							       SENSORS_SUBFEATURE_FAN_INPUT);
			low_feature = sensors_get_subfeature(chip_name,
							     main_feature,
							     SENSORS_SUBFEATURE_FAN_MIN);
			// no fan max feature
			high_feature = NULL;
			break;
		case SENSORS_FEATURE_TEMP:
			input_feature = sensors_get_subfeature(chip_name,
							       main_feature,
							       SENSORS_SUBFEATURE_TEMP_INPUT);
			low_feature = sensors_get_subfeature(chip_name,
							     main_feature,
							     SENSORS_SUBFEATURE_TEMP_MIN);
			high_feature = sensors_get_subfeature(chip_name,
							      main_feature,
							      SENSORS_SUBFEATURE_TEMP_MAX);
			if (!high_feature)
				high_feature = sensors_get_subfeature(chip_name,
								      main_feature,
								      SENSORS_SUBFEATURE_TEMP_CRIT);
			break;
		default:
			g_warning("libsensors plugin: error determining type for sensor '%s'",
				  chip_name_string);
			goto out;
		}

		if (!input_feature)
		{
			g_warning("libsensors plugin: could not get input subfeature for sensor '%s'",
				  chip_name_string);
			goto out;
		}
		// if still here we got input feature so get label
		label = sensors_get_label(chip_name, main_feature);
		if (!label)
		{
			g_warning("libsensors plugin: could not get label for sensor '%s'",
				  chip_name_string);
			goto out;
		}

		g_assert(chip_name_string && label);

		if (low_feature) {
			sensors_get_value(chip_name, low_feature->number, &low);
		}

		if (high_feature) {
			sensors_get_value(chip_name, high_feature->number, &high);
		}
		if (sensors_get_value(chip_name, input_feature->number, &value) < 0) {
			g_warning("libsensors plugin: could not get value for input feature of sensor '%s'",
				  chip_name_string);
			free(label);
			goto out;
		}

		id = g_strdup_printf("%s/%d", chip_name_string,
				     input_feature->number);
		switch (main_feature->type)
		{
		case SENSORS_FEATURE_IN:
		case SENSORS_FEATURE_FAN:
			sensor = is_sensor_new("libsensors", id, label,
					       low, high, "U");
			break;
		case SENSORS_FEATURE_TEMP:
			sensor = is_temperature_sensor_new_full("libsensors",
								id,
								label,
								low, high,
								IS_TEMPERATURE_SENSOR_UNITS_CELSIUS);
			break;
		default:
			g_assert_not_reached();
		}
		/* take ownership of id pointer */
		g_hash_table_insert(priv->sensor_chip_names, id, (void *)chip_name);
		/* connect to update-value signal */
		g_signal_connect(sensor, "update-value",
				 G_CALLBACK(update_sensor_value),
				 self);
		is_indicator_add_sensor(priv->indicator, sensor);
		free(label);
	}
	g_free(chip_name_string);
out:
	return;
}

static void
is_libsensors_plugin_activate(PeasActivatable *activatable)
{
	IsLibsensorsPlugin *self = IS_LIBSENSORS_PLUGIN(activatable);
	IsLibsensorsPluginPrivate *priv = self->priv;
	const sensors_chip_name *chip_name;
	int i = 0;
        int nr = 0;

	/* search for sensors and add them to indicator */
	if (!priv->inited) {
		g_warning("libsensors is not inited, unable to find sensors");
		goto out;
	}
	g_debug("searching for sensors");
	while ((chip_name = sensors_get_detected_chips(NULL, &nr)))
        {
		process_sensors_chip_name(self, chip_name);
	}
out:
	return;
}

static void
is_libsensors_plugin_deactivate(PeasActivatable *activatable)
{
	IsLibsensorsPlugin *plugin = IS_LIBSENSORS_PLUGIN(activatable);
	IsLibsensorsPluginPrivate *priv = plugin->priv;

	/* remove sensors from indicator */
	is_indicator_remove_all_sensors(priv->indicator, "libsensors");
}

static void
is_libsensors_plugin_class_init(IsLibsensorsPluginClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

	g_type_class_add_private(klass, sizeof(IsLibsensorsPluginPrivate));

	gobject_class->get_property = is_libsensors_plugin_get_property;
	gobject_class->set_property = is_libsensors_plugin_set_property;
	gobject_class->finalize = is_libsensors_plugin_finalize;

	g_object_class_override_property(gobject_class, PROP_OBJECT, "object");
}

static void
peas_activatable_iface_init(PeasActivatableInterface *iface)
{
  iface->activate = is_libsensors_plugin_activate;
  iface->deactivate = is_libsensors_plugin_deactivate;
}

static void
is_libsensors_plugin_class_finalize(IsLibsensorsPluginClass *klass)
{
	/* nothing to do */
}

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  is_libsensors_plugin_register_type(G_TYPE_MODULE(module));

  peas_object_module_register_extension_type(module,
					     PEAS_TYPE_ACTIVATABLE,
					     IS_TYPE_LIBSENSORS_PLUGIN);
}

/*  Copyright (c) 2003-2012 Xfce Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <sys/stat.h>

#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>

#include "weather-parsers.h"
#include "weather-data.h"
#include "weather.h"

#include "weather-translate.h"
#include "weather-summary.h"
#include "weather-config.h"
#include "weather-icon.h"
#include "weather-scrollbox.h"
#include "weather-debug.h"

#define XFCEWEATHER_ROOT "weather"
#define UPDATE_INTERVAL (15)
#define DATA_MAX_AGE (20 * 60)
#define CACHE_FILE_MAX_AGE (48 * 3600)
#define BORDER (8)
#define CONNECTION_TIMEOUT (10)        /* connection timeout in seconds */

#define DATA_AND_UNIT(var, item)                                    \
    value = get_data(conditions, data->units, item, data->round);   \
    unit = get_unit(data->units, item);                             \
    var = g_strdup_printf("%s%s%s",                                 \
                          value,                                    \
                          strcmp(unit, "°") ? " " : "",             \
                          unit);                                    \
    g_free(value);

#define CACHE_APPEND(str, val)                  \
    if (val)                                    \
        g_string_append_printf(out, str, val);

#define CACHE_FREE_VARS()                       \
    g_free(locname);                            \
    g_free(lat);                                \
    g_free(lon);                                \
    if (keyfile)                                \
        g_key_file_free(keyfile);

#define CACHE_READ_STRING(var, key)                             \
    if (g_key_file_has_key(keyfile, group, key, NULL))          \
        var = g_key_file_get_string(keyfile, group, key, NULL);


gboolean debug_mode = FALSE;

static void
write_cache_file(plugin_data *data);


void
weather_http_queue_request(SoupSession *session,
                           const gchar *uri,
                           SoupSessionCallback callback_func,
                           gpointer user_data)
{
    SoupMessage *msg;

    msg = soup_message_new("GET", uri);
    soup_session_queue_message(session, msg, callback_func, user_data);
}


static gchar *
make_label(const plugin_data *data,
           data_types type)
{
    xml_time *conditions;
    const gchar *lbl, *unit;
    gchar *str, *value, *rawvalue;

    switch (type) {
    case TEMPERATURE:
        /* TRANSLATORS: Keep in sync with labeloptions in weather-config.c */
        lbl = _("T");
        break;
    case PRESSURE:
        lbl = _("P");
        break;
    case WIND_SPEED:
        lbl = _("WS");
        break;
    case WIND_BEAUFORT:
        lbl = _("WB");
        break;
    case WIND_DIRECTION:
        lbl = _("WD");
        break;
    case WIND_DIRECTION_DEG:
        lbl = _("WD");
        break;
    case HUMIDITY:
        lbl = _("H");
        break;
    case CLOUDS_LOW:
        lbl = _("CL");
        break;
    case CLOUDS_MED:
        lbl = _("CM");
        break;
    case CLOUDS_HIGH:
        lbl = _("CH");
        break;
    case CLOUDINESS:
        lbl = _("C");
        break;
    case FOG:
        lbl = _("F");
        break;
    case PRECIPITATIONS:
        lbl = _("R");
        break;
    default:
        lbl = "?";
        break;
    }

    /* get current weather conditions */
    conditions = get_current_conditions(data->weatherdata);
    rawvalue = get_data(conditions, data->units, type, data->round);

    switch (type) {
    case WIND_DIRECTION:
        value = translate_wind_direction(rawvalue);
        break;
    default:
        value = NULL;
        break;
    }

    if (data->labels->len > 1) {
        if (value != NULL) {
            str = g_strdup_printf("%s: %s", lbl, value);
            g_free(value);
        } else {
            unit = get_unit(data->units, type);
            str = g_strdup_printf("%s: %s%s%s", lbl, rawvalue,
                                  strcmp(unit, "°") ? " " : "", unit);
        }
    } else {
        if (value != NULL) {
            str = g_strdup_printf("%s", value);
            g_free(value);
        } else {
            unit = get_unit(data->units, type);
            str = g_strdup_printf("%s%s%s", rawvalue,
                                  strcmp(unit, "°") ? " " : "", unit);
        }
    }
    g_free(rawvalue);
    return str;
}


/*
 * Return the weather plugin cache directory, creating it if
 * necessary. The string returned does not contain a trailing slash.
 */
gchar *
get_cache_directory(void)
{
    gchar *dir = g_strconcat(g_get_user_cache_dir(), G_DIR_SEPARATOR_S,
                             "xfce4", G_DIR_SEPARATOR_S, "weather",
                             NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}


void
update_icon(plugin_data *data)
{
    GdkPixbuf *icon = NULL;
    xml_time *conditions;
    gchar *str;
    gint size;

    size = data->size;
#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    /* make icon double-size in deskbar mode */
    if (data->panel_orientation == XFCE_PANEL_PLUGIN_MODE_DESKBAR &&
        data->size != data->panel_size)
        size *= 2;
#endif

    /* set icon according to current weather conditions */
    conditions = get_current_conditions(data->weatherdata);
    str = get_data(conditions, data->units, SYMBOL, data->round);
    icon = get_icon(data->icon_theme, str, size, data->night_time);
    g_free(str);
    gtk_image_set_from_pixbuf(GTK_IMAGE(data->iconimage), icon);
    if (G_LIKELY(icon))
        g_object_unref(G_OBJECT(icon));
    weather_debug("Updated panel icon.");
}


void
scrollbox_set_visible(plugin_data *data)
{
    if (data->show_scrollbox && data->labels->len > 0)
        gtk_widget_show_all(GTK_WIDGET(data->vbox_center_scrollbox));
    else
        gtk_widget_hide_all(GTK_WIDGET(data->vbox_center_scrollbox));
}


void
update_scrollbox(plugin_data *data)
{
    GString *out;
    gchar *single = NULL;
    data_types type;
    guint i = 0, j = 0;

    gtk_scrollbox_clear(GTK_SCROLLBOX(data->scrollbox));
    gtk_scrollbox_set_animate(GTK_SCROLLBOX(data->scrollbox),
                              data->scrollbox_animate);

    if (data->weatherdata && data->weatherdata->current_conditions) {
        while (i < data->labels->len) {
            j = 0;
            out = g_string_sized_new(128);
            while ((i + j) < data->labels->len && j < data->scrollbox_lines) {
                type = g_array_index(data->labels, data_types, i + j);
                single = make_label(data, type);
                g_string_append_printf(out, "%s%s", single,
                                       (j < (data->scrollbox_lines - 1)
                                        ? "\n"
                                        : ""));
                g_free(single);
                j++;
            }
            gtk_scrollbox_set_label(GTK_SCROLLBOX(data->scrollbox),
                                    -1, out->str);
            g_string_free(out, TRUE);
            i = i + j;
        }
        weather_debug("Added %u labels to scrollbox.", data->labels->len);
    } else {
        single = g_strdup(_("No Data"));
        gtk_scrollbox_set_label(GTK_SCROLLBOX(data->scrollbox), -1, single);
        g_free(single);
        weather_debug("No weather data available, set single label '%s'.",
                      _("No Data"));
    }

    /* show or hide scrollbox */
    scrollbox_set_visible(data);

    weather_debug("Updated scrollbox.");
}


static void
update_current_conditions(plugin_data *data)
{
    struct tm now_tm;

    if (G_UNLIKELY(data->weatherdata == NULL)) {
        update_icon(data);
        update_scrollbox(data);
        return;
    }

    if (data->weatherdata->current_conditions) {
        xml_time_free(data->weatherdata->current_conditions);
        data->weatherdata->current_conditions = NULL;
    }
    /* use exact 5 minute intervals for calculation */
    time(&data->last_conditions_update);
    now_tm = *localtime(&data->last_conditions_update);
    if (now_tm.tm_min % 5 < 5)
        now_tm.tm_min -= (now_tm.tm_min % 5);
    if (now_tm.tm_min < 0)
        now_tm.tm_min = 0;
    now_tm.tm_sec = 0;
    data->last_conditions_update = mktime(&now_tm);

    data->weatherdata->current_conditions =
        make_current_conditions(data->weatherdata,
                                data->last_conditions_update);
    data->night_time = is_night_time(data->astrodata);
    update_icon(data);
    update_scrollbox(data);
    weather_debug("Updated current conditions.");
}


static void
cb_astro_update(SoupSession *session,
                SoupMessage *msg,
                gpointer user_data)
{
    plugin_data *data = user_data;
    xml_astro *astro;

    if ((astro =
         (xml_astro *) parse_xml_document(msg, (XmlParseFunc) parse_astro))) {
        if (data->astrodata)
            xml_astro_free(data->astrodata);
        data->astrodata = astro;
        data->last_astro_update = time(NULL);
    }
    weather_dump(weather_dump_astrodata, data->astrodata);
}


static void
cb_weather_update(SoupSession *session,
                  SoupMessage *msg,
                  gpointer user_data)
{
    plugin_data *data = user_data;
    xmlDoc *doc;
    xmlNode *root_node;

    weather_debug("Processing downloaded weather data.");
    doc = get_xml_document(msg);
    if (G_LIKELY(doc)) {
        root_node = xmlDocGetRootElement(doc);
        if (G_LIKELY(root_node)) {
            parse_weather(root_node, data->weatherdata);
            data->last_data_update = time(NULL);
        }
        xmlFreeDoc(doc);
    }
    xml_weather_clean(data->weatherdata);
    weather_debug("Updating current conditions.");
    update_current_conditions(data);
    write_cache_file(data);
    weather_dump(weather_dump_weatherdata, data->weatherdata);
}


static gboolean
need_astro_update(const plugin_data *data)
{
    time_t now_t;
    struct tm now_tm, last_tm;

    if (!data->updatetimeout || !data->last_astro_update)
        return TRUE;

    time(&now_t);
    now_tm = *localtime(&now_t);
    last_tm = *localtime(&(data->last_astro_update));
    if (now_tm.tm_mday != last_tm.tm_mday)
        return TRUE;

    return FALSE;
}


static gboolean
need_data_update(const plugin_data *data)
{
    time_t now_t;
    gint diff;

    if (!data->updatetimeout || !data->last_data_update)
        return TRUE;

    time(&now_t);
    diff = (gint) difftime(now_t, data->last_data_update);
    if (diff >= DATA_MAX_AGE)
        return TRUE;

    return FALSE;
}


static gboolean
need_conditions_update(const plugin_data *data)
{
    time_t now_t;
    struct tm now_tm;

    if (!data->updatetimeout || !data->last_conditions_update)
        return TRUE;

    time(&now_t);
    now_tm = *localtime(&now_t);
    return (difftime(now_t, data->last_conditions_update) > 300 &&
            (now_tm.tm_min % 5 == 0 || now_tm.tm_min == 0));
}


static gboolean
update_weatherdata(plugin_data *data)
{
    gchar *url;
    gboolean night_time;
    time_t now_t;
    struct tm now_tm;

    g_assert(data != NULL);
    if (G_UNLIKELY(data == NULL))
        return TRUE;

    if ((!data->lat || !data->lon) ||
        strlen(data->lat) == 0 || strlen(data->lon) == 0) {
        update_icon(data);
        update_scrollbox(data);
        return TRUE;
    }

    /* fetch astronomical data */
    if (need_astro_update(data)) {
        now_t = time(NULL);
        now_tm = *localtime(&now_t);

        /* build url */
        url = g_strdup_printf("http://api.yr.no/weatherapi/sunrise/1.0/?"
                              "lat=%s;lon=%s;date=%04d-%02d-%02d",
                              data->lat, data->lon,
                              now_tm.tm_year + 1900,
                              now_tm.tm_mon + 1,
                              now_tm.tm_mday);

        /* start receive thread */
        g_message("getting %s", url);
        weather_http_queue_request(data->session, url, cb_astro_update, data);
        g_free(url);
    }

    /* fetch weather data */
    if (need_data_update(data)) {
        /* build url */
        url =
            g_strdup_printf("http://api.yr.no/weatherapi"
                            "/locationforecastlts/1.1/?lat=%s;lon=%s;msl=%d",
                            data->lat, data->lon, data->msl);

        /* start receive thread */
        g_message("getting %s", url);
        weather_http_queue_request(data->session, url,
                                   cb_weather_update, data);
        g_free(url);

        /* cb_update will deal with everything that follows this
         * block, so let's return instead of doing things twice */
        return TRUE;
    }

    /* update current conditions, icon and labels */
    if (need_conditions_update(data)) {
        weather_debug("Updating current conditions.");
        update_current_conditions(data);
    }

    /* update night time status and icon */
    night_time = is_night_time(data->astrodata);
    if (data->night_time != night_time) {
        weather_debug("Night time status changed, updating icon.");
        data->night_time = night_time;
        update_icon(data);
    }

    /* keep timeout running */
    return TRUE;
}


GArray *
labels_clear(GArray *array)
{
    if (!array || array->len > 0) {
        if (array)
            g_array_free(array, TRUE);
        array = g_array_new(FALSE, TRUE, sizeof(data_types));
    }
    return array;
}


static void
xfceweather_read_config(XfcePanelPlugin *plugin,
                        plugin_data *data)
{
    XfceRc *rc;
    const gchar *value;
    gchar *file;
    gchar label[10];
    guint label_count = 0;
    gint val;

    if (!(file = xfce_panel_plugin_lookup_rc_file(plugin)))
        return;

    rc = xfce_rc_simple_open(file, TRUE);
    g_free(file);

    if (!rc)
        return;

    value = xfce_rc_read_entry(rc, "loc_name", NULL);
    if (value) {
        if (data->location_name)
            g_free(data->location_name);

        data->location_name = g_strdup(value);
    }

    value = xfce_rc_read_entry(rc, "lat", NULL);
    if (value) {
        if (data->lat)
            g_free(data->lat);

        data->lat = g_strdup(value);
    }

    value = xfce_rc_read_entry(rc, "lon", NULL);
    if (value) {
        if (data->lon)
            g_free(data->lon);

        data->lon = g_strdup(value);
    }

    data->msl = xfce_rc_read_int_entry(rc, "msl", 0);

    data->timezone = xfce_rc_read_int_entry(rc, "timezone", 0);

    data->cache_file_max_age =
        xfce_rc_read_int_entry(rc, "cache_file_max_age", CACHE_FILE_MAX_AGE);

    if (data->units)
        g_slice_free(units_config, data->units);
    data->units = g_slice_new0(units_config);
    data->units->temperature =
        xfce_rc_read_int_entry(rc, "units_temperature", CELSIUS);
    data->units->pressure =
        xfce_rc_read_int_entry(rc, "units_pressure", HECTOPASCAL);
    data->units->windspeed =
        xfce_rc_read_int_entry(rc, "units_windspeed", KMH);
    data->units->precipitations =
        xfce_rc_read_int_entry(rc, "units_precipitations", MILLIMETERS);
    data->units->altitude =
        xfce_rc_read_int_entry(rc, "units_altitude", METERS);

    data->round = xfce_rc_read_bool_entry(rc, "round", TRUE);

    data->tooltip_style = xfce_rc_read_int_entry(rc, "tooltip_style",
                                                 TOOLTIP_VERBOSE);

    val = xfce_rc_read_int_entry(rc, "forecast_layout", FC_LAYOUT_LIST);
    if (val == FC_LAYOUT_CALENDAR || val == FC_LAYOUT_LIST)
        data->forecast_layout = val;
    else
        data->forecast_layout = FC_LAYOUT_LIST;

    val = xfce_rc_read_int_entry(rc, "forecast_days", DEFAULT_FORECAST_DAYS);
    data->forecast_days =
        (val > 0 && val <= MAX_FORECAST_DAYS) ? val : DEFAULT_FORECAST_DAYS;

    value = xfce_rc_read_entry(rc, "theme_dir", NULL);
    if (data->icon_theme)
        icon_theme_free(data->icon_theme);
    data->icon_theme = icon_theme_load(value);

    data->show_scrollbox = xfce_rc_read_bool_entry(rc, "show_scrollbox", TRUE);

    data->scrollbox_lines = xfce_rc_read_int_entry(rc, "scrollbox_lines", 1);
    if (data->scrollbox_lines < 1 ||
        data->scrollbox_lines > MAX_SCROLLBOX_LINES)
        data->scrollbox_lines = 1;

    value = xfce_rc_read_entry(rc, "scrollbox_font", NULL);
    if (value) {
        g_free(data->scrollbox_font);
        data->scrollbox_font = g_strdup(value);
    }

    value = xfce_rc_read_entry(rc, "scrollbox_color", NULL);
    if (value)
        gdk_color_parse("#rrrrggggbbbb", &(data->scrollbox_color));

    data->scrollbox_use_color =
        xfce_rc_read_bool_entry(rc, "scrollbox_use_color", FALSE);

    data->scrollbox_animate =
        xfce_rc_read_bool_entry(rc, "scrollbox_animate", TRUE);
    gtk_scrollbox_set_animate(GTK_SCROLLBOX(data->scrollbox),
                              data->scrollbox_animate);

    data->labels = labels_clear(data->labels);
    val = 0;
    while (val != -1) {
        g_snprintf(label, 10, "label%d", label_count++);

        val = xfce_rc_read_int_entry(rc, label, -1);
        if (val >= 0)
            g_array_append_val(data->labels, val);
    }

    xfce_rc_close(rc);
    weather_debug("Config file read.");
}


static void
xfceweather_write_config(XfcePanelPlugin *plugin,
                         plugin_data *data)
{
    gchar label[10];
    guint i;
    XfceRc *rc;
    gchar *file, *value;

    if (!(file = xfce_panel_plugin_save_location(plugin, TRUE)))
        return;

    /* get rid of old values */
    unlink(file);

    rc = xfce_rc_simple_open(file, FALSE);
    g_free(file);

    if (!rc)
        return;

    if (data->location_name)
        xfce_rc_write_entry(rc, "loc_name", data->location_name);

    if (data->lat)
        xfce_rc_write_entry(rc, "lat", data->lat);

    if (data->lon)
        xfce_rc_write_entry(rc, "lon", data->lon);

    xfce_rc_write_int_entry(rc, "msl", data->msl);

    xfce_rc_write_int_entry(rc, "timezone", data->timezone);

    xfce_rc_write_int_entry(rc, "cache_file_max_age",
                            data->cache_file_max_age);

    xfce_rc_write_int_entry(rc, "units_temperature", data->units->temperature);
    xfce_rc_write_int_entry(rc, "units_pressure", data->units->pressure);
    xfce_rc_write_int_entry(rc, "units_windspeed", data->units->windspeed);
    xfce_rc_write_int_entry(rc, "units_precipitations",
                            data->units->precipitations);
    xfce_rc_write_int_entry(rc, "units_altitude", data->units->altitude);

    xfce_rc_write_bool_entry(rc, "round", data->round);

    xfce_rc_write_int_entry(rc, "tooltip_style", data->tooltip_style);

    xfce_rc_write_int_entry(rc, "forecast_layout", data->forecast_layout);

    xfce_rc_write_int_entry(rc, "forecast_days", data->forecast_days);

    xfce_rc_write_bool_entry(rc, "scrollbox_animate", data->scrollbox_animate);

    if (data->icon_theme && data->icon_theme->dir)
        xfce_rc_write_entry(rc, "theme_dir", data->icon_theme->dir);

    xfce_rc_write_bool_entry(rc, "show_scrollbox",
                             data->show_scrollbox);

    xfce_rc_write_int_entry(rc, "scrollbox_lines", data->scrollbox_lines);

    if (data->scrollbox_font)
        xfce_rc_write_entry(rc, "scrollbox_font", data->scrollbox_font);

    value = gdk_color_to_string(&(data->scrollbox_color));
    xfce_rc_write_entry(rc, "scrollbox_color", value);
    g_free(value);

    xfce_rc_write_bool_entry(rc, "scrollbox_use_color",
                             data->scrollbox_use_color);

    for (i = 0; i < data->labels->len; i++) {
        g_snprintf(label, 10, "label%d", i);
        xfce_rc_write_int_entry(rc, label,
                                (gint) g_array_index(data->labels,
                                                     data_types, i));
    }

    xfce_rc_close(rc);
    weather_debug("Config file written.");
}


/*
 * Generate file name for the weather data cache file.
 */
static gchar *
make_cache_filename(plugin_data *data)
{
    gchar *cache_dir, *file;

    if (G_UNLIKELY(data->lat == NULL || data->lon == NULL))
        return NULL;

    cache_dir = get_cache_directory();
    file = g_strdup_printf("%s%sweatherdata_%s_%s_%d",
                           cache_dir, G_DIR_SEPARATOR_S,
                           data->lat, data->lon, data->msl);
    g_free(cache_dir);
    return file;
}


/*
 * Convert localtime to gmtime and format it to a string
 * that can be parsed later by parse_timestring.
 */
static gchar *
cache_file_strftime_t(const time_t t)
{
    struct tm *tm;
    gchar *res;
    gchar str[21];
    size_t size;

    tm = gmtime(&t);
    size = strftime(str, 21, "%Y-%m-%dT%H:%M:%SZ", tm);
    return (size ? g_strdup(str) : g_strdup(""));
}


static void
write_cache_file(plugin_data *data)
{
    GString *out;
    xml_weather *wd = data->weatherdata;
    xml_time *timeslice;
    xml_location *loc;
    gchar *file, *start, *end, *point, *now;
    time_t now_t = time(NULL);
    guint i, j;

    file = make_cache_filename(data);
    if (G_UNLIKELY(file == NULL))
        return;

    out = g_string_sized_new(20480);
    g_string_assign(out, "# xfce4-weather-plugin cache file\n\n[info]\n");
    CACHE_APPEND("location_name=%s\n", data->location_name);
    CACHE_APPEND("lat=%s\n", data->lat);
    CACHE_APPEND("lon=%s\n", data->lon);
    g_string_append_printf(out, "msl=%d\ntimezone=%d\ntimeslices=%d\n",
                           data->msl, data->timezone, wd->timeslices->len);
    now = cache_file_strftime_t(now_t);
    CACHE_APPEND("cache_date=%s\n\n", now);
    g_free(now);

    for (i = 0; i < wd->timeslices->len; i++) {
        timeslice = g_array_index(wd->timeslices, xml_time *, i);
        if (G_UNLIKELY(timeslice == NULL || timeslice->location == NULL))
            continue;
        loc = timeslice->location;
        start = cache_file_strftime_t(timeslice->start);
        end = cache_file_strftime_t(timeslice->end);
        point = cache_file_strftime_t(timeslice->point);
        g_string_append_printf(out, "[timeslice%d]\n", i);
        CACHE_APPEND("start=%s\n", start);
        CACHE_APPEND("end=%s\n", end);
        CACHE_APPEND("point=%s\n", point);
        CACHE_APPEND("altitude=%s\n", loc->altitude);
        CACHE_APPEND("latitude=%s\n", loc->latitude);
        CACHE_APPEND("longitude=%s\n", loc->longitude);
        CACHE_APPEND("temperature_value=%s\n", loc->temperature_value);
        CACHE_APPEND("temperature_unit=%s\n", loc->temperature_unit);
        CACHE_APPEND("wind_dir_deg=%s\n", loc->wind_dir_deg);
        CACHE_APPEND("wind_dir_name=%s\n", loc->wind_dir_name);
        CACHE_APPEND("wind_speed_mps=%s\n", loc->wind_speed_mps);
        CACHE_APPEND("wind_speed_beaufort=%s\n", loc->wind_speed_beaufort);
        CACHE_APPEND("humidity_value=%s\n", loc->humidity_value);
        CACHE_APPEND("humidity_unit=%s\n", loc->humidity_unit);
        CACHE_APPEND("pressure_value=%s\n", loc->pressure_value);
        CACHE_APPEND("pressure_unit=%s\n", loc->pressure_unit);
        g_free(start);
        g_free(end);
        g_free(point);
        for (j = 0; j < CLOUDS_PERC_NUM; j++)
            g_string_append_printf(out, "clouds_percent[%d]=%s\n", j,
                                   loc->clouds_percent[j]);
        CACHE_APPEND("fog_percent=%s\n", loc->fog_percent);
        CACHE_APPEND("precipitation_value=%s\n", loc->precipitation_value);
        CACHE_APPEND("precipitation_unit=%s\n", loc->precipitation_unit);
        if (loc->symbol)
            g_string_append_printf(out, "symbol_id=%d\nsymbol=%s\n",
                                   loc->symbol_id, loc->symbol);
        g_string_append(out, "\n");
    }

    if (!g_file_set_contents(file, out->str, -1, NULL))
        g_warning("Error writing cache file %s!", file);
    else
        weather_debug("Cache file %s has been written.", file);

    g_string_free(out, TRUE);
    g_free(file);
}


static void
read_cache_file(plugin_data *data)
{
    GKeyFile *keyfile;
    GError **err;
    xml_weather *wd;
    xml_time *timeslice = NULL;
    xml_location *loc = NULL;
    time_t now_t = time(NULL), cache_date_t;
    gchar *file, *locname = NULL, *lat = NULL, *lon = NULL, *group = NULL;
    gchar *timestring;
    gint msl, timezone, num_timeslices, i, j;

    g_assert(data != NULL);
    if (G_UNLIKELY(data == NULL))
        return;
    wd = data->weatherdata;

    if (G_UNLIKELY(data->lat == NULL || data->lon == NULL))
        return;

    file = make_cache_filename(data);
    if (G_UNLIKELY(file == NULL))
        return;

    keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, file, G_KEY_FILE_NONE, NULL)) {
        weather_debug("Could not read cache file %s.", file);
        g_free(file);
        return;
    }
    weather_debug("Reading cache file %s.", file);
    g_free(file);

    group = "info";
    if (!g_key_file_has_group(keyfile, group)) {
        CACHE_FREE_VARS();
        return;
    }

    /* check all needed values are present and match the current parameters */
    locname = g_key_file_get_string(keyfile, group, "location_name", NULL);
    lat = g_key_file_get_string(keyfile, group, "lat", NULL);
    lon = g_key_file_get_string(keyfile, group, "lon", NULL);
    if (locname == NULL || lat == NULL || lon == NULL) {
        CACHE_FREE_VARS();
        weather_debug("Required values are missing in the cache file, "
                      "reading cache file aborted.");
        return;
    }
    msl = g_key_file_get_integer(keyfile, group, "msl", err);
    if (!err)
        timezone = g_key_file_get_integer(keyfile, group, "timezone", err);
    if (!err)
        num_timeslices = g_key_file_get_integer(keyfile, group,
                                                "timeslices", err);
    if (err || strcmp(lat, data->lat) || strcmp(lon, data->lon) ||
        msl != data->msl || timezone != data->timezone || num_timeslices < 1) {
        CACHE_FREE_VARS();
        weather_debug("The required values are not present in the cache file "
                      "or do not match the current plugin data. Reading "
                      "cache file aborted.");
        return;
    }
    /* read cache creation date and check if cache file is not too old */
    CACHE_READ_STRING(timestring, "cache_date");
    cache_date_t = parse_timestring(timestring, NULL);
    g_free(timestring);
    if (difftime(now_t, cache_date_t) > data->cache_file_max_age) {
        weather_debug("Cache file is too old and will not be used.");
        CACHE_FREE_VARS();
        return;
    }
    group = NULL;

    /* parse available timeslices */
    for (i = 0; i < num_timeslices; i++) {
        group = g_strdup_printf("timeslice%d", i);
        if (!g_key_file_has_group(keyfile, group)) {
            weather_debug("Group %s not found, continuing with next.", group);
            g_free(group);
            continue;
        }

        timeslice = make_timeslice();
        if (G_UNLIKELY(timeslice == NULL)) {
            g_free(group);
            continue;
        }

        /* parse time strings (start, end, point) */
        CACHE_READ_STRING(timestring, "start");
        timeslice->start = parse_timestring(timestring, NULL);
        g_free(timestring);
        CACHE_READ_STRING(timestring, "end");
        timeslice->end = parse_timestring(timestring, NULL);
        g_free(timestring);
        CACHE_READ_STRING(timestring, "point");
        timeslice->point = parse_timestring(timestring, NULL);
        g_free(timestring);

        /* parse location data */
        loc = timeslice->location;
        CACHE_READ_STRING(loc->altitude, "altitude");
        CACHE_READ_STRING(loc->latitude, "latitude");
        CACHE_READ_STRING(loc->longitude, "longitude");
        CACHE_READ_STRING(loc->temperature_value, "temperature_value");
        CACHE_READ_STRING(loc->temperature_unit, "temperature_unit");
        CACHE_READ_STRING(loc->wind_dir_deg, "wind_dir_deg");
        CACHE_READ_STRING(loc->wind_speed_mps, "wind_speed_mps");
        CACHE_READ_STRING(loc->wind_speed_beaufort, "wind_speed_beaufort");
        CACHE_READ_STRING(loc->humidity_value, "humidity_value");
        CACHE_READ_STRING(loc->humidity_unit, "humidity_unit");
        CACHE_READ_STRING(loc->pressure_value, "pressure_value");
        CACHE_READ_STRING(loc->pressure_unit, "pressure_unit");

        for (j = 0; j < CLOUDS_PERC_NUM; j++) {
            gchar *key = g_strdup_printf("clouds_percent[%d]", j);
            if (g_key_file_has_key(keyfile, group, key, NULL))
                loc->clouds_percent[j] =
                    g_key_file_get_string(keyfile, group, key, NULL);
            g_free(key);
        }

        CACHE_READ_STRING(loc->fog_percent, "fog_percent");
        CACHE_READ_STRING(loc->precipitation_value, "precipitation_value");
        CACHE_READ_STRING(loc->precipitation_unit, "precipitation_unit");
        CACHE_READ_STRING(loc->symbol, "symbol");
        if (loc->symbol &&
            g_key_file_has_key(keyfile, group, "symbol_id", NULL))
            loc->symbol_id =
                g_key_file_get_integer(keyfile, group, "symbol_id", NULL);

        merge_timeslice(wd, timeslice);
        xml_time_free(timeslice);
    }
    CACHE_FREE_VARS();
    weather_debug("Reading cache file complete.");
}


void
update_weatherdata_with_reset(plugin_data *data, gboolean clear)
{
    weather_debug("Update weatherdata with reset.");
    g_assert(data != NULL);
    if (G_UNLIKELY(data == NULL))
        return;

    if (data->updatetimeout) {
        g_source_remove(data->updatetimeout);
        data->updatetimeout = 0;
    }

    memset(&data->last_data_update, 0, sizeof(data->last_data_update));
    memset(&data->last_astro_update, 0, sizeof(data->last_astro_update));
    memset(&data->last_conditions_update, 0,
           sizeof(data->last_conditions_update));

    /* clear existing weather data, needed for location changes */
    if (clear && data->weatherdata) {
        xml_weather_free(data->weatherdata);
        data->weatherdata = make_weather_data();

        /* make use of previously saved data */
        read_cache_file(data);
    }

    update_weatherdata(data);

    data->updatetimeout =
        g_timeout_add_seconds(UPDATE_INTERVAL,
                              (GSourceFunc) update_weatherdata,
                              data);
    weather_debug("Updated weatherdata with reset.");
}


static void
close_summary(GtkWidget *widget,
              gpointer *user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    if (data->summary_details)
        summary_details_free(data->summary_details);
    data->summary_details = NULL;
    data->summary_window = NULL;
}


void
forecast_click(GtkWidget *widget,
               gpointer user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    if (data->summary_window != NULL)
        gtk_widget_destroy(data->summary_window);
    else {
        data->summary_window = create_summary_window(data);
        g_signal_connect(G_OBJECT(data->summary_window), "destroy",
                         G_CALLBACK(close_summary), data);
        gtk_widget_show_all(data->summary_window);
    }
}


static gboolean
cb_click(GtkWidget *widget,
         GdkEventButton *event,
         gpointer user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    if (event->button == 1)
        forecast_click(widget, user_data);
    else if (event->button == 2)
        update_weatherdata_with_reset(data, FALSE);

    return FALSE;
}


static gboolean
cb_scroll(GtkWidget *widget,
          GdkEventScroll *event,
          gpointer user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    if (event->direction == GDK_SCROLL_UP ||
        event->direction == GDK_SCROLL_DOWN)
        gtk_scrollbox_next_label(GTK_SCROLLBOX(data->scrollbox));

    return FALSE;
}


static void
mi_click(GtkWidget *widget,
         gpointer user_data)
{
    plugin_data *data = (plugin_data *) user_data;

    update_weatherdata_with_reset(data, FALSE);
}


static void
xfceweather_dialog_response(GtkWidget *dlg,
                            gint response,
                            xfceweather_dialog *dialog)
{
    plugin_data *data = (plugin_data *) dialog->pd;
    icon_theme *theme;
    gboolean result;
    guint i;

    if (response == GTK_RESPONSE_HELP) {
        /* show help */
        result = g_spawn_command_line_async("exo-open --launch WebBrowser "
                                            PLUGIN_WEBSITE, NULL);

        if (G_UNLIKELY(result == FALSE))
            g_warning(_("Unable to open the following url: %s"),
                      PLUGIN_WEBSITE);
    } else {
        /* free stuff used in config dialog */
        gtk_widget_destroy(dlg);
        gtk_list_store_clear(dialog->model_datatypes);
        for (i = 0; i < dialog->icon_themes->len; i++) {
            theme = g_array_index(dialog->icon_themes, icon_theme *, i);
            icon_theme_free(theme);
            g_array_free(dialog->icon_themes, TRUE);
        }
        g_slice_free(xfceweather_dialog, dialog);

        xfce_panel_plugin_unblock_menu(data->plugin);

        weather_debug("Saving configuration options.");
        xfceweather_write_config(data->plugin, data);
        weather_dump(weather_dump_plugindata, data);
    }
}


static void
xfceweather_create_options(XfcePanelPlugin *plugin,
                           plugin_data *data)
{
    GtkWidget *dlg, *vbox;
    xfceweather_dialog *dialog;

    xfce_panel_plugin_block_menu(plugin);

    dlg = xfce_titled_dialog_new_with_buttons(_("Weather Update"),
                                              GTK_WINDOW
                                              (gtk_widget_get_toplevel
                                               (GTK_WIDGET(plugin))),
                                              GTK_DIALOG_DESTROY_WITH_PARENT |
                                              GTK_DIALOG_NO_SEPARATOR,
                                              GTK_STOCK_HELP, GTK_RESPONSE_HELP,
                                              GTK_STOCK_CLOSE, GTK_RESPONSE_OK,
                                              NULL);

    gtk_container_set_border_width(GTK_CONTAINER(dlg), 2);
    gtk_window_set_icon_name(GTK_WINDOW(dlg), "xfce4-settings");

    vbox = gtk_vbox_new(FALSE, BORDER);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), BORDER - 2);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), vbox, TRUE, TRUE, 0);

    dialog = create_config_dialog(data, vbox);
    g_signal_connect(G_OBJECT(dlg), "response",
                     G_CALLBACK(xfceweather_dialog_response), dialog);
    gtk_widget_show(dlg);
}


static gchar *
weather_get_tooltip_text(const plugin_data *data)
{
    xml_time *conditions;
    struct tm *point_tm, *start_tm, *end_tm, *sunrise_tm, *sunset_tm;
    gchar *text, *sym, *symbol, *alt, *lat, *lon, *temp;
    gchar *windspeed, *windbeau, *winddir, *winddir_trans, *winddeg;
    gchar *pressure, *humidity, *precipitations;
    gchar *fog, *cloudiness, *sunval, *value;
    gchar sunrise[40], sunset[40];
    gchar point[40], interval_start[40], interval_end[40];
    const gchar *unit;

    conditions = get_current_conditions(data->weatherdata);
    if (G_UNLIKELY(conditions == NULL)) {
        text = g_strdup(_("Short-term forecast data unavailable."));
        return text;
    }

    /* times for forecast and point data */
    point_tm = localtime(&conditions->point);
    strftime(point, 40, "%X", point_tm);
    start_tm = localtime(&conditions->start);
    strftime(interval_start, 40, "%X", start_tm);
    end_tm = localtime(&conditions->end);
    strftime(interval_end, 40, "%X", end_tm);

    /* use sunrise and sunset times if available */
    if (data->astrodata)
        if (data->astrodata->sun_never_rises) {
            sunval = g_strdup(_("The sun never rises today."));
        } else if (data->astrodata->sun_never_sets) {
            sunval = g_strdup(_("The sun never sets today."));
        } else {
            sunrise_tm = localtime(&data->astrodata->sunrise);
            strftime(sunrise, 40, "%X", sunrise_tm);
            sunset_tm = localtime(&data->astrodata->sunset);
            strftime(sunset, 40, "%X", sunset_tm);
            sunval = g_strdup_printf(_("The sun rises at %s and sets at %s."),
                                     sunrise, sunset);
        }
    else
        sunval = g_strdup_printf("");

    sym = get_data(conditions, data->units, SYMBOL, FALSE);
    DATA_AND_UNIT(symbol, SYMBOL);
    DATA_AND_UNIT(alt, ALTITUDE);
    DATA_AND_UNIT(lat, LATITUDE);
    DATA_AND_UNIT(lon, LONGITUDE);
    DATA_AND_UNIT(temp, TEMPERATURE);
    DATA_AND_UNIT(windspeed, WIND_SPEED);
    DATA_AND_UNIT(windbeau, WIND_BEAUFORT);
    DATA_AND_UNIT(winddir, WIND_DIRECTION);
    winddir_trans = translate_wind_direction(winddir);
    DATA_AND_UNIT(winddeg, WIND_DIRECTION_DEG);
    DATA_AND_UNIT(pressure, PRESSURE);
    DATA_AND_UNIT(humidity, HUMIDITY);
    DATA_AND_UNIT(precipitations, PRECIPITATIONS);
    DATA_AND_UNIT(fog, FOG);
    DATA_AND_UNIT(cloudiness, CLOUDINESS);

    switch (data->tooltip_style) {
    case TOOLTIP_SIMPLE:
        text = g_markup_printf_escaped
            /*
             * TRANSLATORS: This is the simple tooltip. For a bigger challenge,
             * look at the verbose tooltip style further below ;-)
             */
            (_("<b><span size=\"large\">%s</span></b> "
               "<span size=\"medium\">(%s)</span>\n"
               "<b><span size=\"large\">%s</span></b>\n\n"
               "<b>Temperature:</b> %s\n"
               "<b>Wind:</b> %s from %s\n"
               "<b>Pressure:</b> %s\n"
               "<b>Humidity:</b> %s\n"),
             data->location_name, alt,
             translate_desc(sym, data->night_time),
             temp, windspeed, winddir_trans, pressure, humidity);
        break;

    case TOOLTIP_VERBOSE:
    default:
        text = g_markup_printf_escaped
            /*
             * TRANSLATORS: Re-arrange and align at will, optionally using
             * abbreviations for labels if desired or necessary. Just take
             * into account the possible size constraints, the centered
             * vertical alignment of the icon - which unfortunately cannot
             * be changed easily - and try to make it compact and look
             * good!
             */
            (_("<b><span size=\"large\">%s</span></b> "
               "<span size=\"medium\">(%s)</span>\n"
               "<b><span size=\"large\">%s</span></b>\n"
               "<span size=\"smaller\">"
               "from %s to %s, with %s precipitations</span>\n\n"
               "<b>Temperature:</b> %s\t\t"
               "<span size=\"smaller\">(values at %s)</span>\n"
               "<b>Wind:</b> %s (%son the Beaufort scale) from %s(%s)\n"
               "<b>Pressure:</b> %s    <b>Humidity:</b> %s\n"
               "<b>Fog:</b> %s    <b>Cloudiness:</b> %s\n\n"
               "<span size=\"smaller\">%s</span>"),
             data->location_name,
             alt,
             translate_desc(sym, data->night_time),
             interval_start, interval_end,
             precipitations,
             temp, point,
             windspeed, windbeau, winddir_trans, winddeg,
             pressure, humidity,
             fog, cloudiness,
             sunval);
        break;
    }
    g_free(sunval);
    g_free(sym);
    g_free(symbol);
    g_free(alt);
    g_free(lat);
    g_free(lon);
    g_free(temp);
    g_free(windspeed);
    g_free(windbeau);
    g_free(winddir_trans);
    g_free(winddir);
    g_free(winddeg);
    g_free(pressure);
    g_free(humidity);
    g_free(precipitations);
    g_free(fog);
    g_free(cloudiness);
    return text;
}


static gboolean
weather_get_tooltip_cb(GtkWidget *widget,
                       gint x,
                       gint y,
                       gboolean keyboard_mode,
                       GtkTooltip *tooltip,
                       plugin_data *data)
{
    GdkPixbuf *icon;
    xml_time *conditions;
    gchar *markup_text, *rawvalue;
    guint icon_size;

    if (data->weatherdata == NULL)
        gtk_tooltip_set_text(tooltip, _("Cannot update weather data"));
    else {
        markup_text = weather_get_tooltip_text(data);
        gtk_tooltip_set_markup(tooltip, markup_text);
        g_free(markup_text);
    }

    conditions = get_current_conditions(data->weatherdata);
    rawvalue = get_data(conditions, data->units, SYMBOL, data->round);
    switch (data->tooltip_style) {
    case TOOLTIP_SIMPLE:
        icon_size = 96;
        break;
    case TOOLTIP_VERBOSE:
    default:
        icon_size = 128;
        break;
    }
    icon = get_icon(data->icon_theme, rawvalue, icon_size, data->night_time);
    g_free(rawvalue);
    gtk_tooltip_set_icon(tooltip, icon);
    g_object_unref(G_OBJECT(icon));

    return TRUE;
}


static plugin_data *
xfceweather_create_control(XfcePanelPlugin *plugin)
{
    plugin_data *data = g_slice_new0(plugin_data);
    SoupMessage *msg;
    SoupURI *soup_proxy_uri;
    const gchar *proxy_uri;
    GtkWidget *refresh;
    data_types lbl;
    GdkPixbuf *icon = NULL;

    /* Initialize with sane default values */
    data->plugin = plugin;
    data->units = g_slice_new0(units_config);
    data->weatherdata = make_weather_data();
    data->cache_file_max_age = CACHE_FILE_MAX_AGE;
    data->show_scrollbox = TRUE;
    data->scrollbox_lines = 1;
    data->scrollbox_animate = TRUE;
    data->tooltip_style = TOOLTIP_VERBOSE;
    data->forecast_layout = FC_LAYOUT_LIST;
    data->forecast_days = DEFAULT_FORECAST_DAYS;
    data->round = TRUE;

    /* Setup session for HTTP connections */
    data->session = soup_session_async_new();
    g_object_set(data->session, SOUP_SESSION_TIMEOUT,
                 CONNECTION_TIMEOUT, NULL);

    /* Set the proxy URI from environment */
    proxy_uri = g_getenv("HTTP_PROXY");
    if (!proxy_uri)
        proxy_uri = g_getenv("http_proxy");
    if (proxy_uri) {
        soup_proxy_uri = soup_uri_new (proxy_uri);
        g_object_set(data->session, SOUP_SESSION_PROXY_URI,
                     soup_proxy_uri, NULL);
        soup_uri_free(soup_proxy_uri);
    }

    data->scrollbox = gtk_scrollbox_new();

    data->size = xfce_panel_plugin_get_size(plugin);
    data->icon_theme = icon_theme_load(NULL);
    icon = get_icon(data->icon_theme, NULL, 16, FALSE);
    if (G_LIKELY(icon)) {
        data->iconimage = gtk_image_new_from_pixbuf(icon);
        g_object_unref(G_OBJECT(icon));
    } else
        g_warning("No default icon theme? "
                  "This should not happen, plugin will crash!");

    data->labels = g_array_new(FALSE, TRUE, sizeof(data_types));

    data->vbox_center_scrollbox = gtk_vbox_new(FALSE, 0);
    data->top_hbox = gtk_hbox_new(FALSE, 0);
    gtk_misc_set_alignment(GTK_MISC(data->iconimage), 1, 0.5);
    gtk_box_pack_start(GTK_BOX(data->top_hbox),
                       data->iconimage, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(data->vbox_center_scrollbox),
                       data->scrollbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(data->top_hbox),
                       data->vbox_center_scrollbox, TRUE, TRUE, 0);

    data->top_vbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(data->top_vbox),
                       data->top_hbox, TRUE, FALSE, 0);

    data->tooltipbox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(data->tooltipbox), data->top_vbox);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(data->tooltipbox), FALSE);
    gtk_widget_show_all(data->tooltipbox);

    g_object_set(G_OBJECT(data->tooltipbox), "has-tooltip", TRUE, NULL);
    g_signal_connect(G_OBJECT(data->tooltipbox), "query-tooltip",
                     G_CALLBACK(weather_get_tooltip_cb),
                     data);
    xfce_panel_plugin_add_action_widget(plugin, data->tooltipbox);

    g_signal_connect(G_OBJECT(data->tooltipbox), "button-press-event",
                     G_CALLBACK(cb_click), data);
    g_signal_connect(G_OBJECT(data->tooltipbox), "scroll-event",
                     G_CALLBACK(cb_scroll), data);
    gtk_widget_add_events(data->scrollbox, GDK_BUTTON_PRESS_MASK);

    /* add refresh button to right click menu, for people who missed
       the middle mouse click feature */
    refresh = gtk_image_menu_item_new_from_stock("gtk-refresh", NULL);
    gtk_widget_show(refresh);

    g_signal_connect(G_OBJECT(refresh), "activate",
                     G_CALLBACK(mi_click), data);

    xfce_panel_plugin_menu_insert_item(plugin, GTK_MENU_ITEM(refresh));

    /* assign to tempval because g_array_append_val() is using & operator */
    lbl = TEMPERATURE;
    g_array_append_val(data->labels, lbl);
    lbl = WIND_DIRECTION;
    g_array_append_val(data->labels, lbl);
    lbl = WIND_SPEED;
    g_array_append_val(data->labels, lbl);

    /*
     * FIXME: Without this the first label looks odd, because
     * the gc isn't created yet
     */
    gtk_scrollbox_set_label(GTK_SCROLLBOX(data->scrollbox), -1, "1");
    gtk_scrollbox_clear(GTK_SCROLLBOX(data->scrollbox));

    data->updatetimeout =
        g_timeout_add_seconds(UPDATE_INTERVAL,
                              (GSourceFunc) update_weatherdata,
                              data);

    weather_debug("Plugin widgets set up and ready.");
    return data;
}


static void
xfceweather_free(XfcePanelPlugin *plugin,
                 plugin_data *data)
{
    weather_debug("Freeing plugin data.");
    g_assert(data != NULL);

    if (data->weatherdata)
        xml_weather_free(data->weatherdata);

    if (data->astrodata)
        xml_astro_free(data->astrodata);

    if (data->units)
        g_slice_free(units_config, data->units);

    if (data->updatetimeout) {
        g_source_remove(data->updatetimeout);
        data->updatetimeout = 0;
    }

    xmlCleanupParser();

    /* free chars */
    g_free(data->lat);
    g_free(data->lon);
    g_free(data->location_name);
    g_free(data->scrollbox_font);

    /* free array */
    g_array_free(data->labels, TRUE);

    /* free icon theme */
    icon_theme_free(data->icon_theme);

    g_slice_free(plugin_data, data);
    data = NULL;
}


static gboolean
xfceweather_set_size(XfcePanelPlugin *panel,
                     gint size,
                     plugin_data *data)
{
    data->panel_size = size;
#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    size /= xfce_panel_plugin_get_nrows(panel);
#endif
    data->size = size;

    update_icon(data);
    update_scrollbox(data);

    weather_dump(weather_dump_plugindata, data);

    /* we handled the size */
    return TRUE;
}


#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
static gboolean
xfceweather_set_mode(XfcePanelPlugin *panel,
                     XfcePanelPluginMode mode,
                     plugin_data *data)
{
    GtkWidget *parent = gtk_widget_get_parent(data->vbox_center_scrollbox);

    data->panel_orientation = xfce_panel_plugin_get_mode(panel);
    data->orientation = (mode != XFCE_PANEL_PLUGIN_MODE_VERTICAL)
        ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;

    g_object_ref(G_OBJECT(data->vbox_center_scrollbox));
    gtk_container_remove(GTK_CONTAINER(parent), data->vbox_center_scrollbox);

    if (data->panel_orientation == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL)
        gtk_box_pack_start(GTK_BOX(data->top_hbox),
                           data->vbox_center_scrollbox, TRUE, FALSE, 0);
    else
        gtk_box_pack_start(GTK_BOX(data->top_vbox),
                           data->vbox_center_scrollbox, TRUE, FALSE, 0);
    g_object_unref(G_OBJECT(data->vbox_center_scrollbox));

    if (data->panel_orientation == XFCE_PANEL_PLUGIN_MODE_DESKBAR)
        xfce_panel_plugin_set_small(XFCE_PANEL_PLUGIN(panel), FALSE);
    else
        xfce_panel_plugin_set_small(XFCE_PANEL_PLUGIN(panel), TRUE);

    gtk_scrollbox_set_orientation(GTK_SCROLLBOX(data->scrollbox),
                                  data->orientation);

    update_icon(data);
    update_scrollbox(data);

    weather_dump(weather_dump_plugindata, data);

    /* we handled the orientation */
    return TRUE;
}


#else


static gboolean
xfceweather_set_orientation(XfcePanelPlugin *panel,
                            GtkOrientation orientation,
                            plugin_data *data)
{
    GtkWidget *parent = gtk_widget_get_parent(data->vbox_center_scrollbox);

    data->orientation = GTK_ORIENTATION_HORIZONTAL;
    data->panel_orientation = orientation;

    g_object_ref(G_OBJECT(data->vbox_center_scrollbox));
    gtk_container_remove(GTK_CONTAINER(parent), data->vbox_center_scrollbox);

    if (data->panel_orientation == GTK_ORIENTATION_HORIZONTAL)
        gtk_box_pack_start(GTK_BOX(data->top_hbox),
                           data->vbox_center_scrollbox, TRUE, FALSE, 0);
    else
        gtk_box_pack_start(GTK_BOX(data->top_vbox),
                           data->vbox_center_scrollbox, TRUE, FALSE, 0);
    g_object_unref(G_OBJECT(data->vbox_center_scrollbox));

    gtk_scrollbox_set_orientation(GTK_SCROLLBOX(data->scrollbox),
                                  data->panel_orientation);

    update_icon(data);
    update_scrollbox(data);

    weather_dump(weather_dump_plugindata, data);

    /* we handled the orientation */
    return TRUE;
}
#endif


static void
xfceweather_show_about(XfcePanelPlugin *plugin,
                       plugin_data *data)
{
    GdkPixbuf *icon;
    const gchar *auth[] = {
        "Bob Schlärmann <weatherplugin@atreidis.nl.eu.org>",
        "Benedikt Meurer <benny@xfce.org>",
        "Jasper Huijsmans <jasper@xfce.org>",
        "Masse Nicolas <masse_nicolas@yahoo.fr>",
        "Nick Schermer <nick@xfce.org>",
        "Colin Leroy <colin@colino.net>",
        "Harald Judt <h.judt@gmx.at>",
        NULL };
    icon = xfce_panel_pixbuf_from_source("xfce4-weather", NULL, 48);
    gtk_show_about_dialog
        (NULL,
         "logo", icon,
         "license", xfce_get_license_text(XFCE_LICENSE_TEXT_GPL),
         "version", PACKAGE_VERSION,
         "program-name", PACKAGE_NAME,
         "comments", _("Show weather conditions and forecasts"),
         "website", PLUGIN_WEBSITE,
         "copyright", _("Copyright (c) 2003-2012\n"),
         "authors", auth,
         NULL);

    if (icon)
        g_object_unref(G_OBJECT(icon));
}


static void
weather_construct(XfcePanelPlugin *plugin)
{
    plugin_data *data;
    const gchar *panel_debug_env;

    /* Enable debug level logging if PANEL_DEBUG contains G_LOG_DOMAIN */
    panel_debug_env = g_getenv("PANEL_DEBUG");
    if (panel_debug_env && strstr(panel_debug_env, G_LOG_DOMAIN))
        debug_mode = TRUE;
    weather_debug_init(G_LOG_DOMAIN, debug_mode);
    weather_debug("weather plugin version " VERSION " starting up");

    xfce_textdomain(GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");
    data = xfceweather_create_control(plugin);
    xfceweather_read_config(plugin, data);
    read_cache_file(data);
    scrollbox_set_visible(data);
    gtk_scrollbox_set_fontname(GTK_SCROLLBOX(data->scrollbox),
                               data->scrollbox_font);
    if (data->scrollbox_use_color)
        gtk_scrollbox_set_color(GTK_SCROLLBOX(data->scrollbox),
                                data->scrollbox_color);

#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    xfceweather_set_mode(plugin, xfce_panel_plugin_get_mode(plugin), data);
#else
    xfceweather_set_orientation(plugin, xfce_panel_plugin_get_orientation(plugin), data);
#endif
    xfceweather_set_size(plugin, xfce_panel_plugin_get_size(plugin), data);

    gtk_container_add(GTK_CONTAINER(plugin), data->tooltipbox);

    g_signal_connect(G_OBJECT(plugin), "free-data",
                     G_CALLBACK(xfceweather_free), data);
    g_signal_connect(G_OBJECT(plugin), "save",
                     G_CALLBACK(xfceweather_write_config), data);
    g_signal_connect(G_OBJECT(plugin), "size-changed",
                     G_CALLBACK(xfceweather_set_size), data);
#if LIBXFCE4PANEL_CHECK_VERSION(4,9,0)
    g_signal_connect(G_OBJECT(plugin), "mode-changed",
                     G_CALLBACK(xfceweather_set_mode), data);
#else
    g_signal_connect(G_OBJECT(plugin), "orientation-changed",
                     G_CALLBACK(xfceweather_set_orientation), data);
#endif
    xfce_panel_plugin_menu_show_configure(plugin);
    g_signal_connect(G_OBJECT(plugin), "configure-plugin",
                     G_CALLBACK(xfceweather_create_options), data);

    xfce_panel_plugin_menu_show_about(plugin);
    g_signal_connect(G_OBJECT(plugin), "about",
                     G_CALLBACK(xfceweather_show_about), data);

    weather_dump(weather_dump_plugindata, data);

    update_weatherdata(data);
}

XFCE_PANEL_PLUGIN_REGISTER(weather_construct)

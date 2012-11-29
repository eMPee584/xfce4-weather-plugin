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
#include <libxfce4ui/libxfce4ui.h>

#include "weather-parsers.h"
#include "weather-data.h"
#include "weather.h"
#include "weather-debug.h"
#include "weather-config.h"
#include "weather-search.h"
#include "weather-scrollbox.h"

#define GEONAMES_USERNAME "xfce4weatherplugin"
#define UPDATE_TIMER_DELAY 7
#define OPTIONS_N 13
#define BORDER 4
#define LOC_NAME_MAX_LEN 50

#define ADD_PAGE(homogenous)                                        \
    palign = gtk_alignment_new(0.5, 0.5, 1, 1);                     \
    gtk_container_set_border_width(GTK_CONTAINER(palign), BORDER);  \
    page = gtk_vbox_new(homogenous, BORDER);                        \
    gtk_container_add(GTK_CONTAINER(palign), page);

#define ADD_LABEL(text, sg)                                         \
    label = gtk_label_new_with_mnemonic(text);                      \
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);                \
    if (G_LIKELY(sg))                                               \
        gtk_size_group_add_widget(sg, label);                       \
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, BORDER);

#define ADD_SPIN(spin, min, max, step, val, digits, sg)                 \
    spin = gtk_spin_button_new_with_range(min, max, step);              \
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), val);              \
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);          \
    if (G_LIKELY(sg))                                                   \
        gtk_size_group_add_widget(sg, spin);                            \
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), GTK_WIDGET(spin));  \
    gtk_box_pack_start(GTK_BOX(hbox), spin, FALSE, FALSE, 0);

#define ADD_COMBO(combo)                                                \
    combo = gtk_combo_box_new_text();                                   \
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), GTK_WIDGET(combo)); \
    gtk_box_pack_start(GTK_BOX(hbox), combo, TRUE, TRUE, 0);

#define ADD_COMBO_VALUE(combo, text)                        \
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), text);

#define SET_COMBO_VALUE(combo, val)                         \
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), val);

#define ADD_LABEL_EDIT_BUTTON(button, text, icon, cb_func)          \
    button = gtk_button_new_with_mnemonic(text);                    \
    image = gtk_image_new_from_stock(icon, GTK_ICON_SIZE_BUTTON);   \
    gtk_button_set_image(GTK_BUTTON(button), image);                \
    gtk_size_group_add_widget(sg_button, button);                   \
    g_signal_connect(G_OBJECT(button), "clicked",                   \
                     G_CALLBACK(cb_func), dialog);


static const labeloption labeloptions[OPTIONS_N] = {
    /*
     * TRANSLATORS: The abbreviations in parentheses will be shown in
     * the scrollbox together with the values. Keep them in sync with
     * those in make_label() in weather.c. Some of them may be
     * standardized internationally, like CL, CM, CH, and you might
     * read that up somewhere and decide whether you want to use them
     * or not. In general, though, you should just try to choose
     * letter(s) that make sense and don't use up too much space.
     */
    {N_("Temperature (T)"), TEMPERATURE},
    {N_("Atmosphere pressure (P)"), PRESSURE},
    {N_("Wind speed (WS)"), WIND_SPEED},
    {N_("Wind speed - Beaufort scale (WB)"), WIND_BEAUFORT},
    {N_("Wind direction (WD)"), WIND_DIRECTION},
    {N_("Wind direction in degrees (WD)"), WIND_DIRECTION_DEG},
    {N_("Humidity (H)"), HUMIDITY},
    {N_("Low clouds (CL)"), CLOUDS_LOW},
    {N_("Medium clouds (CM)"), CLOUDS_MED},
    {N_("High clouds (CH)"), CLOUDS_HIGH},
    {N_("Cloudiness (C)"), CLOUDINESS},
    {N_("Fog (F)"), FOG},
    {N_("Precipitations (R)"), PRECIPITATIONS},
};

static void
spin_alt_value_changed(const GtkWidget *spin,
                       gpointer user_data);


static void
update_summary_window(xfceweather_dialog *dialog,
                      gboolean restore_position)
{
    if (dialog->wd->summary_window) {
        guint x, y;

        /* remember position */
        if (restore_position)
            gtk_window_get_position(GTK_WINDOW(dialog->wd->summary_window),
                                    &x, &y);

        /* call toggle function two times to close and open dialog */
        forecast_click(dialog->wd->summary_window, dialog->wd);
        forecast_click(dialog->wd->summary_window, dialog->wd);

        /* ask wm to restore position of new window */
        if (restore_position)
            gtk_window_move(GTK_WINDOW(dialog->wd->summary_window), x, y);

        /* bring config dialog to the front, it might have been hidden
         * beneath the summary window */
        gtk_window_present(GTK_WINDOW(dialog->dialog));
    }
}


static gboolean
schedule_data_update(gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (dialog == NULL)
        return FALSE;

    /* force update of downloaded data */
    weather_debug("Delayed update timer expired, now scheduling data update.");
    update_weatherdata_with_reset(dialog->wd);
    gtk_spinner_stop(GTK_SPINNER(dialog->update_spinner));
    gtk_widget_hide(GTK_WIDGET(dialog->update_spinner));
    return FALSE;
}


static void
schedule_delayed_data_update(xfceweather_dialog *dialog)
{
    weather_debug("Starting delayed data update.");
    /* cancel any update that was scheduled before */
    if (dialog->timer_id)
        g_source_remove(dialog->timer_id);

    /* stop any updates that could be performed by weather.c */
    if (dialog->wd->updatetimeout)
        g_source_remove(dialog->wd->updatetimeout);

    gtk_widget_show(GTK_WIDGET(dialog->update_spinner));
    gtk_spinner_start(GTK_SPINNER(dialog->update_spinner));
    dialog->timer_id =
        g_timeout_add_seconds(UPDATE_TIMER_DELAY,
                              (GSourceFunc) schedule_data_update, dialog);
}


static gchar *
sanitize_location_name(const gchar *location_name)
{
    gchar *pos, *pos2, sane[LOC_NAME_MAX_LEN * 4];
    glong len, offset;

    pos = g_utf8_strchr(location_name, -1, ',');
    if (pos != NULL) {
        pos2 = pos;
        while ((pos2 = g_utf8_next_char(pos2)))
            if (g_utf8_get_char(pos2) == ',') {
                pos = pos2;
                break;
            }
        offset = g_utf8_pointer_to_offset(location_name, pos);
        if (offset > LOC_NAME_MAX_LEN)
            offset = LOC_NAME_MAX_LEN;
        (void) g_utf8_strncpy(sane, location_name, offset);
        sane[LOC_NAME_MAX_LEN * 4 - 1] = '\0';
        return g_strdup(sane);
    } else {
        len = g_utf8_strlen(location_name, LOC_NAME_MAX_LEN);

        if (len >= LOC_NAME_MAX_LEN) {
            (void) g_utf8_strncpy(sane, location_name, len);
            sane[LOC_NAME_MAX_LEN * 4 - 1] = '\0';
            return g_strdup(sane);
        }

        if (len > 0)
            return g_strdup(location_name);

        return g_strdup(_("Unset"));
    }
}


static void
cb_lookup_altitude(SoupSession *session,
                   SoupMessage *msg,
                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    xml_altitude *altitude = NULL;
    gdouble alt;

    altitude = (xml_altitude *)
        parse_xml_document(msg, (XmlParseFunc) parse_altitude);

    if (altitude) {
        alt = string_to_double(altitude->altitude, -9999);
        weather_debug("Altitude returned by GeoNames: %.0f meters", alt);
        if (alt >= -420) {
            if (dialog->wd->units->altitude == FEET)
                alt /= 0.3048;
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt),
                                      alt);
        }
        xml_altitude_free(altitude);
    }
}


static void
cb_lookup_timezone(SoupSession *session,
                   SoupMessage *msg,
                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    xml_timezone *timezone = NULL;
    gint tz;

    timezone = (xml_timezone *)
        parse_xml_document(msg, (XmlParseFunc) parse_timezone);
    weather_dump(weather_dump_timezone, timezone);

    if (timezone) {
        tz = (gint) string_to_double(timezone->offset, -9999);
        if (tz != -9999)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_timezone),
                                      tz);
        xml_timezone_free(timezone);
    }
}


static void
lookup_altitude_timezone(const gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gchar *url, latbuf[10], lonbuf[10];
    gdouble lat, lon;

    lat = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dialog->spin_lat));
    lon = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dialog->spin_lon));

    (void) g_ascii_formatd(latbuf, 10, "%.6f", lat);
    (void) g_ascii_formatd(lonbuf, 10, "%.6f", lon);

    /* lookup altitude */
    url = g_strdup_printf("http://api.geonames.org"
                          "/srtm3XML?lat=%s&lng=%s&username=%s",
                          &latbuf[0], &lonbuf[0], GEONAMES_USERNAME);
    weather_http_queue_request(dialog->wd->session, url,
                               cb_lookup_altitude, user_data);
    g_free(url);

    /* lookup timezone */
    url = g_strdup_printf("http://www.earthtools.org/timezone/%s/%s",
                          &latbuf[0], &lonbuf[0]);
    weather_http_queue_request(dialog->wd->session, url,
                               cb_lookup_timezone, user_data);
    g_free(url);
}


static void
setup_units(xfceweather_dialog *dialog,
            const units_config *units)
{
    if (G_UNLIKELY(units == NULL))
        return;

    SET_COMBO_VALUE(dialog->combo_unit_temperature, units->temperature);
    SET_COMBO_VALUE(dialog->combo_unit_pressure, units->pressure);
    SET_COMBO_VALUE(dialog->combo_unit_windspeed, units->windspeed);
    SET_COMBO_VALUE(dialog->combo_unit_precipitations, units->precipitations);
    SET_COMBO_VALUE(dialog->combo_unit_altitude, units->altitude);
}


static void
auto_locate_cb(const gchar *loc_name,
               const gchar *lat,
               const gchar *lon,
               const units_config *units,
               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (lat && lon && loc_name) {
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), loc_name);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lat),
                                  string_to_double(lat, 0));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lon),
                                  string_to_double(lon, 0));
        lookup_altitude_timezone(user_data);
    } else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Unset"));
    setup_units(dialog, units);
    gtk_widget_set_sensitive(dialog->text_loc_name, TRUE);
}


static void
start_auto_locate(xfceweather_dialog *dialog)
{
    gtk_widget_set_sensitive(dialog->text_loc_name, FALSE);
    gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Detecting..."));
    gtk_spinner_start(GTK_SPINNER(dialog->update_spinner));
    weather_search_by_ip(dialog->wd->session, auto_locate_cb, dialog);
}


static gboolean
cb_findlocation(GtkButton *button,
                gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    search_dialog *sdialog;
    gchar *loc_name;

    sdialog = create_search_dialog(NULL, dialog->wd->session);

    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    if (run_search_dialog(sdialog)) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lat),
                                  string_to_double(sdialog->result_lat, 0));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_lon),
                                  string_to_double(sdialog->result_lon, 0));
        loc_name = sanitize_location_name(sdialog->result_name);
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), loc_name);
        g_free(loc_name);
    }
    free_search_dialog(sdialog);

    lookup_altitude_timezone(user_data);
    gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);

    return FALSE;
}


static void
setup_altitude(xfceweather_dialog *dialog)
{
    g_signal_handlers_block_by_func(dialog->spin_alt,
                                    G_CALLBACK(spin_alt_value_changed),
                                    dialog);
    switch (dialog->wd->units->altitude) {
    case METERS:
        gtk_label_set_text(GTK_LABEL(dialog->label_alt_unit),
                           _("meters"));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  (gdouble) (dialog->wd->msl));
        break;

    case FEET:
        gtk_label_set_text(GTK_LABEL(dialog->label_alt_unit),
                           _("feet"));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->spin_alt),
                                  (gdouble) dialog->wd->msl / 0.3048);
        break;
    }
    g_signal_handlers_unblock_by_func(dialog->spin_alt,
                                      G_CALLBACK(spin_alt_value_changed),
                                      dialog);
    return;
}


static void
text_loc_name_changed(const GtkWidget *spin,
                      gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    g_free(dialog->wd->location_name);
    dialog->wd->location_name =
        g_strdup(gtk_entry_get_text(GTK_ENTRY(dialog->text_loc_name)));
}


static void
spin_lat_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gchar latbuf[10];
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    g_free(dialog->wd->lat);
    dialog->wd->lat = g_strdup(g_ascii_formatd(latbuf, 10, "%.6f", val));
    schedule_delayed_data_update(dialog);
}


static void
spin_lon_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gchar lonbuf[10];
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    g_free(dialog->wd->lon);
    dialog->wd->lon = g_strdup(g_ascii_formatd(lonbuf, 10, "%.6f", val));
    schedule_delayed_data_update(dialog);
}


static void
spin_alt_value_changed(const GtkWidget *spin,
                       gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gdouble val;

    val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (dialog->wd->units->altitude == FEET)
        val *= 0.3048;
    dialog->wd->msl = (gint) val;
    schedule_delayed_data_update(dialog);
}


static void
spin_timezone_value_changed(const GtkWidget *spin,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    dialog->wd->timezone =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
}


static GtkWidget *
create_location_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *vbox, *label, *image;
    GtkWidget *button_loc_change;
    GtkSizeGroup *sg_label, *sg_spin;

    ADD_PAGE(FALSE);
    sg_label = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    sg_spin = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* location name */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Location _name:"), sg_label);
    dialog->text_loc_name = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(dialog->text_loc_name),
                             LOC_NAME_MAX_LEN);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label),
                                  GTK_WIDGET(dialog->text_loc_name));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->text_loc_name, TRUE, TRUE, 0);
    button_loc_change = gtk_button_new_with_mnemonic(_("Chan_ge..."));
    image = gtk_image_new_from_stock(GTK_STOCK_FIND, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(button_loc_change), image);
    g_signal_connect(G_OBJECT(button_loc_change), "clicked",
                     G_CALLBACK(cb_findlocation), dialog);
    gtk_box_pack_start(GTK_BOX(hbox), button_loc_change,
                       FALSE, FALSE, 0);
    if (dialog->wd->location_name)
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name),
                           dialog->wd->location_name);
    else
        gtk_entry_set_text(GTK_ENTRY(dialog->text_loc_name), _("Unset"));
    /* update spinner */
    dialog->update_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(hbox), dialog->update_spinner, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, BORDER);

    /* latitude */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Latitud_e:"), sg_label);
    ADD_SPIN(dialog->spin_lat, -90, 90, 1,
             (string_to_double(dialog->wd->lat, 0)), 6, sg_spin);
    label = gtk_label_new("°");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* longitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("L_ongitude:"), sg_label);
    ADD_SPIN(dialog->spin_lon, -180, 180, 1,
             (string_to_double(dialog->wd->lon, 0)), 6, sg_spin);
    label = gtk_label_new("°");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* altitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Altitude:"), sg_label);
    ADD_SPIN(dialog->spin_alt, -420, 10000, 1, dialog->wd->msl, 0, sg_spin);
    dialog->label_alt_unit = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(dialog->label_alt_unit), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), dialog->label_alt_unit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* timezone */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Timezone:"), sg_label);
    ADD_SPIN(dialog->spin_timezone, -24, 24, 1,
             dialog->wd->timezone, 0, sg_spin);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* instructions for correction of altitude and timezone */
    hbox = gtk_hbox_new(FALSE, BORDER);
    label = gtk_label_new(_("Please change location name to your liking and "
                            "correct\naltitude and timezone if they are "
                            "not auto-detected correctly."));
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, BORDER/2);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER/2);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    /* set up the altitude spin box and unit label (meters/feet) */
    setup_altitude(dialog);

    g_object_unref(G_OBJECT(sg_label));
    g_object_unref(G_OBJECT(sg_spin));
    return palign;
}


static void
combo_unit_temperature_changed(GtkWidget *combo,
                               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->units->temperature =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    update_scrollbox(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_pressure_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->units->pressure =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    update_scrollbox(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_windspeed_changed(GtkWidget *combo,
                             gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->units->windspeed =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    update_scrollbox(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_precipitations_changed(GtkWidget *combo,
                                  gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->units->precipitations =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    update_scrollbox(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static void
combo_unit_altitude_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    dialog->wd->units->altitude =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    setup_altitude(dialog);
    update_summary_window(dialog, TRUE);
}


static GtkWidget *
create_units_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *vbox, *label;
    GtkSizeGroup *sg_label;

    ADD_PAGE(FALSE);
    sg_label = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    vbox = gtk_vbox_new(FALSE, BORDER);

    /* temperature */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Temperature:"), sg_label);
    ADD_COMBO(dialog->combo_unit_temperature);
    ADD_COMBO_VALUE(dialog->combo_unit_temperature, _("Celcius"));
    ADD_COMBO_VALUE(dialog->combo_unit_temperature, _("Fahrenheit"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* atmospheric pressure */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Atmospheric _pressure:"), sg_label);
    ADD_COMBO(dialog->combo_unit_pressure);
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Hectopascals (hPa)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Inches of mercury (inHg)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Pound-force per square inch (psi)"));
    ADD_COMBO_VALUE(dialog->combo_unit_pressure,
                    _("Torr (mmHg)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* wind speed */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Wind speed:"), sg_label);
    ADD_COMBO(dialog->combo_unit_windspeed);
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Kilometers per hour (km/h)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Miles per hour (mph)"));
    ADD_COMBO_VALUE(dialog->combo_unit_windspeed,
                    _("Meters per second (m/s)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* precipitations */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Prec_ipitations:"), sg_label);
    ADD_COMBO(dialog->combo_unit_precipitations);
    ADD_COMBO_VALUE(dialog->combo_unit_precipitations,
                    _("Millimeters (mm)"));
    ADD_COMBO_VALUE(dialog->combo_unit_precipitations,
                    _("Inches (in)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* altitude */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("Altitu_de:"), sg_label);
    ADD_COMBO(dialog->combo_unit_altitude);
    ADD_COMBO_VALUE(dialog->combo_unit_altitude,
                    _("Meters (m)"));
    ADD_COMBO_VALUE(dialog->combo_unit_altitude,
                    _("Feet (ft)"));
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, BORDER);

    /* initialize widgets with current data */
    if (dialog->wd)
        setup_units(dialog, dialog->wd->units);

    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);
    g_object_unref(G_OBJECT(sg_label));
    return palign;
}


static void
combo_icon_theme_changed(GtkWidget *combo,
                         gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    icon_theme *theme;
    gint i;

    i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    if (G_UNLIKELY(i == -1))
        return;

    theme = g_array_index(dialog->icon_themes, icon_theme*, i);
    if (G_UNLIKELY(theme == NULL))
        return;

    icon_theme_free(dialog->wd->icon_theme);
    dialog->wd->icon_theme = icon_theme_copy(theme);
    update_icon(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static void
combo_tooltip_style_changed(GtkWidget *combo,
                            gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->tooltip_style = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
}


static void
combo_forecast_layout_changed(GtkWidget *combo,
                              gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->forecast_layout =
        gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    update_summary_window(dialog, FALSE);
}


static void
spin_forecast_days_value_changed(const GtkWidget *spin,
                                 gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->forecast_days =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    update_summary_window(dialog, FALSE);
}


static void
check_round_values_toggled(GtkWidget *button,
                           gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->round =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    update_scrollbox(dialog->wd);
    update_summary_window(dialog, TRUE);
}


static GtkWidget *
create_appearance_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *vbox, *label;
    GtkSizeGroup *sg;
    icon_theme *theme;
    guint i;

    ADD_PAGE(TRUE);
    sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* icon theme */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Icon theme:"), sg);
    ADD_COMBO(dialog->combo_icon_theme);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    dialog->icon_themes = find_icon_themes();
    for (i = 0; i < dialog->icon_themes->len; i++) {
        theme = g_array_index(dialog->icon_themes, icon_theme*, i);
        ADD_COMBO_VALUE(dialog->combo_icon_theme, theme->name);
        /* set selection to current theme */
        if (G_LIKELY(dialog->wd->icon_theme) &&
            !strcmp(theme->dir, dialog->wd->icon_theme->dir))
            SET_COMBO_VALUE(dialog->combo_icon_theme, i);
    }

    /* tooltip style */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Tooltip style:"), sg);
    ADD_COMBO(dialog->combo_tooltip_style);
    ADD_COMBO_VALUE(dialog->combo_tooltip_style, _("Simple"));
    ADD_COMBO_VALUE(dialog->combo_tooltip_style, _("Verbose"));
    SET_COMBO_VALUE(dialog->combo_tooltip_style, dialog->wd->tooltip_style);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    /* forecast layout */
    vbox = gtk_vbox_new(FALSE, BORDER);
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Forecast layout:"), sg);
    ADD_COMBO(dialog->combo_forecast_layout);
    ADD_COMBO_VALUE(dialog->combo_forecast_layout, _("Days in columns"));
    ADD_COMBO_VALUE(dialog->combo_forecast_layout, _("Days in rows"));
    SET_COMBO_VALUE(dialog->combo_forecast_layout,
                    dialog->wd->forecast_layout);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* number of days shown in forecast */
    hbox = gtk_hbox_new(FALSE, BORDER);
    ADD_LABEL(_("_Number of forecast _days:"), sg);
    ADD_SPIN(dialog->spin_forecast_days, 1, MAX_FORECAST_DAYS, 1,
             (dialog->wd->forecast_days ? dialog->wd->forecast_days : 5),
             0, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    /* round temperature */
    vbox = gtk_vbox_new(FALSE, BORDER);
    dialog->check_round_values =
        gtk_check_button_new_with_mnemonic(_("_Round values"));
    gtk_box_pack_start(GTK_BOX(vbox), dialog->check_round_values,
                       FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dialog->check_round_values),
                                 dialog->wd->round);

    /* interpolate data */
    dialog->check_interpolate_data =
        gtk_check_button_new_with_mnemonic(_("Interpolate _data"));
    gtk_box_pack_start(GTK_BOX(vbox), dialog->check_interpolate_data,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), vbox, FALSE, FALSE, 0);

    g_object_unref(G_OBJECT(sg));
    return palign;
}


static void
check_scrollbox_show_toggled(GtkWidget *button,
                             gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->show_scrollbox =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    scrollbox_set_visible(dialog->wd);
}


static void
spin_scrollbox_lines_value_changed(const GtkWidget *spin,
                                   gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->scrollbox_lines =
        (guint) gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    update_scrollbox(dialog->wd);
}


static gboolean
button_scrollbox_font_pressed(GtkWidget *button,
                              GdkEventButton *event,
                              gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (event->button == 2) {
        g_free(dialog->wd->scrollbox_font);
        dialog->wd->scrollbox_font = NULL;
        gtk_scrollbox_set_fontname(GTK_SCROLLBOX(dialog->wd->scrollbox),
                                   NULL);
        gtk_button_set_label(GTK_BUTTON(button), _("Select _font"));
        return TRUE;
    }

    return FALSE;
}

static gboolean
button_scrollbox_font_clicked(GtkWidget *button,
                              gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkFontSelectionDialog *fsd;
    gchar *fontname;
    gint result;

    fsd = GTK_FONT_SELECTION_DIALOG
        (gtk_font_selection_dialog_new(_("Select font")));
    if (dialog->wd->scrollbox_font)
        gtk_font_selection_dialog_set_font_name(fsd,
                                                dialog->wd->scrollbox_font);

    result = gtk_dialog_run(GTK_DIALOG(fsd));
    if (result == GTK_RESPONSE_OK || result == GTK_RESPONSE_ACCEPT) {
        fontname = gtk_font_selection_dialog_get_font_name(fsd);
        if (fontname != NULL) {
            gtk_button_set_label(GTK_BUTTON(button), fontname);
            g_free(dialog->wd->scrollbox_font);
            dialog->wd->scrollbox_font = fontname;
            gtk_scrollbox_set_fontname(GTK_SCROLLBOX(dialog->wd->scrollbox),
                                       fontname);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(fsd));
    return FALSE;
}


static gboolean
button_scrollbox_color_pressed(GtkWidget *button,
                               GdkEventButton *event,
                               gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkStyle *style;
    GdkColor color;
    gchar *colstr;

    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    if (event->button == 2) {
        dialog->wd->scrollbox_use_color = FALSE;
        gtk_scrollbox_clear_color(GTK_SCROLLBOX(dialog->wd->scrollbox));
        return TRUE;
    }

    return FALSE;
}


static void
button_scrollbox_color_set(GtkWidget *button,
                           gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;

    gtk_color_button_get_color(GTK_COLOR_BUTTON(button),
                               &(dialog->wd->scrollbox_color));
    gtk_scrollbox_set_color(GTK_SCROLLBOX(dialog->wd->scrollbox),
                            dialog->wd->scrollbox_color);
    dialog->wd->scrollbox_use_color = TRUE;
}


static GtkWidget *
make_label(void)
{
    GtkWidget *widget, *menu;
    guint i;

    menu = gtk_menu_new();
    widget = gtk_option_menu_new();
    for (i = 0; i < OPTIONS_N; i++) {
        labeloption opt = labeloptions[i];

        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_menu_item_new_with_label(_(opt.name)));
    }
    gtk_option_menu_set_menu(GTK_OPTION_MENU(widget), menu);
    return widget;
}


static void
update_scrollbox_labels(xfceweather_dialog *dialog)
{
    GtkTreeIter iter;
    gboolean hasiter;
    GValue value = { 0 };
    gint option;

    dialog->wd->labels = labels_clear(dialog->wd->labels);
    hasiter =
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL
                                      (dialog->model_datatypes),
                                      &iter);
    while (hasiter == TRUE) {
        gtk_tree_model_get_value(GTK_TREE_MODEL(dialog->model_datatypes),
                                 &iter, 1, &value);
        option = g_value_get_int(&value);
        g_array_append_val(dialog->wd->labels, option);
        g_value_unset(&value);
        hasiter =
            gtk_tree_model_iter_next(GTK_TREE_MODEL(dialog->model_datatypes),
                                     &iter);
    }
    update_scrollbox(dialog->wd);
}


static void
add_model_option(GtkListStore *model,
                 const gint opt)
{
    GtkTreeIter iter;

    gtk_list_store_append(model, &iter);
    gtk_list_store_set(model, &iter,
                       0, _(labeloptions[opt].name),
                       1, labeloptions[opt].number, -1);
}


static int
option_i(const data_types opt)
{
    guint i;

    for (i = 0; i < OPTIONS_N; i++)
        if (labeloptions[i].number == opt)
            return i;

    return -1;
}


static gboolean
button_add_option_clicked(GtkWidget *widget,
                          gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    gint history;

    history =
        gtk_option_menu_get_history(GTK_OPTION_MENU(dialog->options_datatypes));
    add_model_option(dialog->model_datatypes, history);
    update_scrollbox_labels(dialog);
    return FALSE;
}


static gboolean
button_del_option_clicked(GtkWidget *widget,
                          gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkTreeSelection *selection;
    GtkTreeIter iter;

    selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(dialog->list_datatypes));
    if (gtk_tree_selection_get_selected(selection, NULL, &iter))
        gtk_list_store_remove(GTK_LIST_STORE(dialog->model_datatypes), &iter);
    update_scrollbox_labels(dialog);
    return FALSE;
}


static gboolean
button_up_option_clicked(GtkWidget *widget,
                         gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkTreeSelection *selection;
    GtkTreeIter iter, prev;
    GtkTreePath *path;

    selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(dialog->list_datatypes));
    if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        path = gtk_tree_model_get_path(GTK_TREE_MODEL(dialog->model_datatypes),
                                       &iter);
        if (gtk_tree_path_prev(path)) {
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(dialog->model_datatypes),
                                        &prev, path))
                gtk_list_store_move_before(GTK_LIST_STORE(dialog->model_datatypes),
                                           &iter, &prev);

            gtk_tree_path_free(path);
        }
    }
    update_scrollbox_labels(dialog);
    return FALSE;
}


static gboolean
button_down_option_clicked(GtkWidget *widget,
                           gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    GtkTreeIter iter, next;
    GtkTreeSelection *selection;

    selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(dialog->list_datatypes));
    if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        next = iter;
        if (gtk_tree_model_iter_next(GTK_TREE_MODEL(dialog->model_datatypes),
                                     &next))
            gtk_list_store_move_after(GTK_LIST_STORE(dialog->model_datatypes),
                                      &iter, &next);
    }
    update_scrollbox_labels(dialog);
    return FALSE;
}


static void
check_scrollbox_animate_toggled(GtkWidget *button,
                                gpointer user_data)
{
    xfceweather_dialog *dialog = (xfceweather_dialog *) user_data;
    dialog->wd->scrollbox_animate =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
    gtk_scrollbox_set_animate(GTK_SCROLLBOX(dialog->wd->scrollbox),
                              dialog->wd->scrollbox_animate);
}


static GtkWidget *
create_scrollbox_page(xfceweather_dialog *dialog)
{
    GtkWidget *palign, *page, *hbox, *table, *scroll, *label, *image;
    GtkSizeGroup *sg_misc, *sg_button;
    GtkWidget *button_add, *button_del, *button_up, *button_down;
    GtkTreeViewColumn *column;
    GtkCellRenderer *renderer;
    data_types type;
    guint i;
    gint n;

    ADD_PAGE(FALSE);
    sg_misc = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* show scrollbox */
    hbox = gtk_hbox_new(FALSE, BORDER);
    dialog->check_scrollbox_show =
        gtk_check_button_new_with_mnemonic("Show scroll_box");
    gtk_box_pack_start(GTK_BOX(hbox), dialog->check_scrollbox_show,
                       TRUE, TRUE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON
                                 (dialog->check_scrollbox_show),
                                 dialog->wd->show_scrollbox);

    /* values to show at once (multiple lines) */
    label = gtk_label_new_with_mnemonic(_("L_ines:"));
    gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
    ADD_SPIN(dialog->spin_scrollbox_lines, 1, MAX_SCROLLBOX_LINES, 1,
             dialog->wd->scrollbox_lines, 0, sg_misc);
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    /* font and color */
    hbox = gtk_hbox_new(FALSE, BORDER);
    label = gtk_label_new(_("Font and color:"));
    gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    dialog->button_scrollbox_font =
        gtk_button_new_with_mnemonic(_("Select _font"));
    gtk_box_pack_start(GTK_BOX(hbox), dialog->button_scrollbox_font,
                       TRUE, TRUE, 0);
    if (dialog->wd->scrollbox_font)
        gtk_button_set_label(GTK_BUTTON(dialog->button_scrollbox_font),
                             dialog->wd->scrollbox_font);
    dialog->button_scrollbox_color =
        gtk_color_button_new_with_color(&(dialog->wd->scrollbox_color));
    gtk_size_group_add_widget(sg_misc, dialog->button_scrollbox_color);
    gtk_box_pack_start(GTK_BOX(hbox), dialog->button_scrollbox_color,
                       FALSE, FALSE, 0 );
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    /* labels and buttons */
    hbox = gtk_hbox_new(FALSE, BORDER);
    sg_button = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    dialog->options_datatypes = make_label();
    gtk_box_pack_start(GTK_BOX(hbox), dialog->options_datatypes, TRUE, TRUE, 0);
    dialog->model_datatypes = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
    dialog->list_datatypes =
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(dialog->model_datatypes));
    renderer = gtk_cell_renderer_text_new();
    column =
        gtk_tree_view_column_new_with_attributes(_("Labels to _display"),
                                                 renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(dialog->list_datatypes), column);
    gtk_widget_set_size_request(dialog->options_datatypes, 300, -1);

    /* button "add" */
    ADD_LABEL_EDIT_BUTTON(button_add, _("_Add"),
                          GTK_STOCK_ADD, button_add_option_clicked);
    gtk_box_pack_start(GTK_BOX(hbox), button_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    /* labels to display */
    hbox = gtk_hbox_new(FALSE, BORDER);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), dialog->list_datatypes);
    gtk_box_pack_start(GTK_BOX(hbox), scroll, TRUE, TRUE, 0);

    /* button "remove" */
    table = gtk_table_new(4, 1, TRUE);
    ADD_LABEL_EDIT_BUTTON(button_del, _("_Remove"),
                          GTK_STOCK_REMOVE, button_del_option_clicked);
    gtk_table_attach_defaults(GTK_TABLE(table), button_del, 0, 1, 0, 1);

    /* button "move up" */
    ADD_LABEL_EDIT_BUTTON(button_up, _("Move _up"),
                          GTK_STOCK_GO_UP, button_up_option_clicked);
    gtk_table_attach_defaults(GTK_TABLE(table), button_up, 0, 1, 2, 3);

    /* button "move down" */
    ADD_LABEL_EDIT_BUTTON(button_down, _("Move _down"),
                          GTK_STOCK_GO_DOWN, button_down_option_clicked);
    gtk_table_attach_defaults(GTK_TABLE(table), button_down, 0, 1, 3, 4);

    gtk_box_pack_start(GTK_BOX(hbox), table, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, FALSE, 0);

    if (dialog->wd->labels->len > 0) {
        for (i = 0; i < dialog->wd->labels->len; i++) {
            type = g_array_index(dialog->wd->labels, data_types, i);

            if ((n = option_i(type)) != -1)
                add_model_option(dialog->model_datatypes, n);
        }
    }

    dialog->check_scrollbox_animate =
        gtk_check_button_new_with_mnemonic(_("Animate _transitions between labels"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON
                                 (dialog->check_scrollbox_animate),
                                 dialog->wd->scrollbox_animate);
    gtk_box_pack_start(GTK_BOX(page), dialog->check_scrollbox_animate,
                       FALSE, FALSE, 0);

    g_object_unref(G_OBJECT(sg_misc));
    g_object_unref(G_OBJECT(sg_button));
    return palign;
}


void
setup_notebook_signals(xfceweather_dialog *dialog)
{
    /* location page */
    g_signal_connect(GTK_EDITABLE(dialog->text_loc_name), "changed",
                     G_CALLBACK(text_loc_name_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_lat), "value-changed",
                     G_CALLBACK(spin_lat_value_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_lon), "value-changed",
                     G_CALLBACK(spin_lon_value_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_alt), "value-changed",
                     G_CALLBACK(spin_alt_value_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_timezone), "value-changed",
                     G_CALLBACK(spin_timezone_value_changed), dialog);

    /* units page */
    g_signal_connect(dialog->combo_unit_temperature, "changed",
                     G_CALLBACK(combo_unit_temperature_changed), dialog);
    g_signal_connect(dialog->combo_unit_pressure, "changed",
                     G_CALLBACK(combo_unit_pressure_changed), dialog);
    g_signal_connect(dialog->combo_unit_windspeed, "changed",
                     G_CALLBACK(combo_unit_windspeed_changed), dialog);
    g_signal_connect(dialog->combo_unit_precipitations, "changed",
                     G_CALLBACK(combo_unit_precipitations_changed), dialog);
    g_signal_connect(dialog->combo_unit_altitude, "changed",
                     G_CALLBACK(combo_unit_altitude_changed), dialog);

    /* appearance page */
    g_signal_connect(dialog->combo_icon_theme, "changed",
                     G_CALLBACK(combo_icon_theme_changed), dialog);
    g_signal_connect(dialog->combo_tooltip_style, "changed",
                     G_CALLBACK(combo_tooltip_style_changed), dialog);
    g_signal_connect(dialog->combo_forecast_layout, "changed",
                     G_CALLBACK(combo_forecast_layout_changed), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_forecast_days),
                     "value-changed",
                     G_CALLBACK(spin_forecast_days_value_changed), dialog);
    g_signal_connect(dialog->check_round_values, "toggled",
                     G_CALLBACK(check_round_values_toggled), dialog);

    /* scrollbox page */
    g_signal_connect(dialog->check_scrollbox_show, "toggled",
                     G_CALLBACK(check_scrollbox_show_toggled), dialog);
    g_signal_connect(GTK_SPIN_BUTTON(dialog->spin_scrollbox_lines),
                     "value-changed",
                     G_CALLBACK(spin_scrollbox_lines_value_changed),
                     dialog);
    g_signal_connect(G_OBJECT(dialog->button_scrollbox_font),
                     "button_press_event",
                     G_CALLBACK(button_scrollbox_font_pressed),
                     dialog);
    g_signal_connect(dialog->button_scrollbox_font, "clicked",
                     G_CALLBACK(button_scrollbox_font_clicked), dialog);
    g_signal_connect(G_OBJECT(dialog->button_scrollbox_color),
                     "button-press-event",
                     G_CALLBACK(button_scrollbox_color_pressed),
                     dialog);
    g_signal_connect(dialog->button_scrollbox_color, "color-set",
                     G_CALLBACK(button_scrollbox_color_set), dialog);
    g_signal_connect(dialog->check_scrollbox_animate, "toggled",
                     G_CALLBACK(check_scrollbox_animate_toggled), dialog);
}


xfceweather_dialog *
create_config_dialog(xfceweather_data *data,
                     GtkWidget *vbox)
{
    xfceweather_dialog *dialog;
    GtkWidget *notebook;

    dialog = g_slice_new0(xfceweather_dialog);
    dialog->wd = (xfceweather_data *) data;
    dialog->dialog = gtk_widget_get_toplevel(vbox);

    notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_location_page(dialog),
                             gtk_label_new_with_mnemonic("_Location"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_units_page(dialog),
                             gtk_label_new_with_mnemonic("_Units"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_appearance_page(dialog),
                             gtk_label_new_with_mnemonic("_Appearance"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_scrollbox_page(dialog),
                             gtk_label_new_with_mnemonic("_Scrollbox"));
    setup_notebook_signals(dialog);

    /* automatically detect current location if it is yet unknown */
    if (!(dialog->wd->lat && dialog->wd->lon))
        start_auto_locate(dialog);

    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
    gtk_widget_show_all(vbox);
    gtk_widget_hide(GTK_WIDGET(dialog->update_spinner));
    return dialog;
}

#!/bin/bash


tabs='gui core'


schemafile=$1
num=1
callbackfile=callback.c

# header of the callback function
cat > $callbackfile << EOF

/** generated file, do not edit! */
#ifndef DT_PREFERENCES_H
#define DT_PREFERENCES_H

#include <gtk/gtk.h>
#include "control/conf.h"

static void
preferences_callback (GtkWidget *widget, gpointer user_data)
{
  long int num = (long int)user_data;
  switch(num)
  {
EOF

# do this for each key
# arg short long
function key_begin {
  cat >> $initfile << EOF
  label = gtk_label_new(_("$1"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  labelev = gtk_event_box_new();
  gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(double_click_callback), (gpointer)(long int)- $num);
  gtk_container_add(GTK_CONTAINER(labelev), label);
EOF
}

# arg short long
function key_end {
  cat >> $initfile << EOF
  gtk_object_set(GTK_OBJECT(labelev),  "tooltip-text", _("double click to reset"), NULL);
  gtk_object_set(GTK_OBJECT(widget), "tooltip-text", "$2", NULL);
  gtk_box_pack_start(GTK_BOX(vbox1), labelev, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, TRUE, 0);

EOF
}

# split up in different tabs!
for tab in $tabs
do

initfile=init_$tab.c

# header of the init tab function
cat > $initfile << EOF
static void
init_tab_$tab (GtkWidget *tab)
{
  GtkWidget *widget, *label, *labelev;
  GtkWidget *hbox = gtk_hbox_new(5, FALSE);
  GtkWidget *vbox1 = gtk_vbox_new(5, TRUE);
  GtkWidget *vbox2 = gtk_vbox_new(5, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, FALSE, TRUE, 0);
  gtk_notebook_append_page(GTK_NOTEBOOK(tab), hbox, gtk_label_new(_("$tab")));

EOF

# get key, type, default, short, long for each setting:
# grep for all relevant tags
#  remove spaces
#  remove new lines, insert only the one we want
#  remove the ugly xml tag stuff and the gconf schema prefix.
grep -E '<key>|<type>|<default>|<short>|<long>|<schema>|</schema>' $schemafile |
      sed -e 's/^[ \t]*//;s/[ \t]*$//' |
      tr -d '\n' | sed -e 's/<\/schema>/#/g'  | tr '#' '\n' | grep -E "tab:$tab" |
      sed -e 's/<\/[^>]*>/#/g' -e 's/<[^>]*>//g' -e 's/\/schemas\/apps\/darktable\///g' > dreggn

# now read one line at the time and output code:
# label (short), input (type, default), tooltip (long) with callback (key)

echo "    // tab: $tab" >> $callbackfile

for line0 in $(cat dreggn | tr ' ' '&')
do
  line=$(echo $line0 | tr '&' ' ')
  key=$(echo $line | cut -d# -f1)
  type=$(echo $line | cut -d# -f2)
  def=$(echo $line | cut -d# -f3)
  short=$(echo $line | cut -d# -f4)
  long=$(echo $line | cut -d# -f5)
  key_begin "$short" "$long" $num
  if [ $type == "string" ]; then
    def=\"$def\"
    val="gtk_entry_get_text(GTK_ENTRY(widget))"
    cat >> $initfile << EOF
  widget = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("$key"));
  g_signal_connect(G_OBJECT(widget), "activated", G_CALLBACK(preferences_callback), (gpointer)(long int)$num);
EOF
  elif [ $type == "int" ]; then
    val="gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget))"
    cat >> $initfile << EOF
  widget = gtk_spin_button_new();
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
  gtk_spin_button_set_value(GTK_ENTRY(widget), dt_conf_get_int("$key"));
  g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback), (gpointer)(long int)$num);
EOF
  elif [ $type == "bool" ]; then
    if [ $def == "true" ]; then def="TRUE"; else def="FALSE"; fi
    val="gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))"
    cat >> $initfile << EOF
  widget = gtk_check_button_new();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("$key"));
  g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback), (gpointer)(long int)$num);
EOF
  elif [ $type == "float" ]; then
    val="gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget))"
    cat >> $initfile << EOF
  widget = gtk_spin_button_new();
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 5);
  gtk_spin_button_set_value(GTK_ENTRY(widget), dt_conf_get_float("$key"));
  g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback), (gpointer)(long int)$num);
EOF
  fi
  key_end "$short" "$long" $num

  cat >> $callbackfile << EOF
    case $num:
      dt_conf_set_$type("$key", $val);
      break;
    case -$num:
      dt_conf_set_$type("$key", $def);
      break;
EOF
  num=$[ num+1 ]
done

cat >> $initfile << EOF
}

EOF

done # end for all tabs

cat >> $callbackfile << EOF
    default:
      break;
  }
}

static gboolean 
double_click_callback (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    preferences_callback(widget, user_data);
    return TRUE;
  }
  return FALSE;
}

EOF

cat $callbackfile
rm $callbackfile

for tab in $tabs;
do
  cat init_$tab.c
  rm init_$tab.c
done # end for all tabs

echo "#endif"

rm dreggn


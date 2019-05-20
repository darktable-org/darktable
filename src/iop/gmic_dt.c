/*
    This file is part of darktable,
    copyright (c) 2019 edgardo hoszowski.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/gmic_dt.h"
#include "develop/imageop_math.h"
#include "develop/masks.h"
#include "gui/color_picker_proxy.h"

DT_MODULE_INTROSPECTION(1, dt_iop_gmic_dt_params_t)

#define DT_GMIC_PARAMETERS_LEN 30

typedef struct dt_iop_gmic_parameter_point_t
{
  float x, y;
} dt_iop_gmic_parameter_point_t;

typedef struct dt_iop_gmic_parameter_color_t
{
  float r, g, b;
} dt_iop_gmic_parameter_color_t;

typedef struct dt_iop_gmic_dt_command_parameter_t
{
  int id;
  dt_gmic_params_type_t type;
  union
  {
    float _float;
    int _int;
    gboolean _bool;
    int _choice;
    dt_iop_gmic_parameter_color_t _color;
    dt_iop_gmic_parameter_point_t _point;
  } value;
} dt_iop_gmic_dt_command_parameter_t;

typedef struct dt_iop_gmic_dt_widgets_t
{
  int param_id;
  dt_gmic_params_type_t type;
  GtkWidget *widg;
  GtkWidget *widg2;
} dt_iop_gmic_dt_widgets_t;

typedef struct dt_iop_gmic_dt_params_t
{
  char gmic_command_name[31];
  dt_gmic_colorspaces_t colorspace;
  gboolean scale_image;
  dt_iop_gmic_dt_command_parameter_t gmic_parameters[DT_GMIC_PARAMETERS_LEN];
} dt_iop_gmic_dt_params_t;

typedef struct dt_iop_gmic_dt_gui_data_t
{
  GtkWidget *cmb_gmic_commands;
  GtkWidget *vbox_gmic_params;

  int widget_count;
  dt_iop_gmic_dt_widgets_t *widgets;

  dt_iop_color_picker_t color_picker;

  gboolean draw_overlays;
  int dragging_index;
} dt_iop_gmic_dt_gui_data_t;

typedef struct dt_iop_gmic_dt_params_t dt_iop_gmic_dt_data_t;

const char *name()
{
  return _("gmic");
}

int default_group()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorspace_type_t colorspace = iop_cs_rgb;

  if(piece)
  {
    const dt_iop_gmic_dt_data_t *p = (dt_iop_gmic_dt_data_t *)piece->data;
    if(p->colorspace == DT_GMIC_LAB_3C || p->colorspace == DT_GMIC_LAB_1C) colorspace = iop_cs_Lab;
  }
  else if(self)
  {
    const dt_iop_gmic_dt_data_t *p = (dt_iop_gmic_dt_data_t *)self->params;
    if(p->colorspace == DT_GMIC_LAB_3C || p->colorspace == DT_GMIC_LAB_1C) colorspace = iop_cs_Lab;
  }

  return colorspace;
}

// float --> str, with decimal separator == '.'
static void dt_ftoa(char *str_dest, const float value, const size_t dest_size)
{
  g_snprintf(str_dest, dest_size, "%f", value);
  int i = 0;
  while(str_dest[i])
  {
    if(str_dest[i] == ',')
    {
      str_dest[i] = '.';
      break;
    }
    i++;
  }
}

// returns the first dt_gmic_command_t with name == gmic_command_name
static dt_gmic_command_t *_get_gmic_command_by_name(const char *gmic_command_name)
{
  dt_gmic_command_t *gmic_command = NULL;

  GList *l = g_list_first(darktable.gmic_commands);
  while(l)
  {
    dt_gmic_command_t *command = (dt_gmic_command_t *)(l->data);
    if(strcmp(command->name, gmic_command_name) == 0)
    {
      gmic_command = command;
      break;
    }

    l = g_list_next(l);
  }

  return gmic_command;
}

// returns the index of widget in g->widgets[]
static int _get_param_index_from_widget(GtkWidget *widget, dt_iop_gmic_dt_gui_data_t *g)
{
  int index = -1;
  for(int i = 0; i < g->widget_count; i++)
  {
    if(g->widgets[i].widg == widget || g->widgets[i].widg2 == widget)
    {
      index = i;
      break;
    }
  }
  return index;
}

// returns the widget asociated with param_id
static dt_iop_gmic_dt_widgets_t *_get_param_widget_from_id(const int param_id, dt_iop_gmic_dt_gui_data_t *g)
{
  dt_iop_gmic_dt_widgets_t *widget = NULL;
  for(int i = 0; i < g->widget_count; i++)
  {
    if(g->widgets[i].param_id == param_id)
    {
      widget = g->widgets + i;
      break;
    }
  }
  return widget;
}

// returns the index of the parameter with id=param_id in p->gmic_parameters[]
static int _get_param_index_from_id(const int param_id, const dt_iop_gmic_dt_params_t *p)
{
  int index = -1;
  for(int i = 0; i < DT_GMIC_PARAMETERS_LEN; i++)
  {
    if(p->gmic_parameters[i].id == param_id)
    {
      index = i;
      break;
    }
  }
  return index;
}

// returns the dt_gmic_parameter_t in gmic_command with id=param_id
static dt_gmic_parameter_t *_get_parameter_by_id(const dt_gmic_command_t *gmic_command, const int param_id)
{
  dt_gmic_parameter_t *parameter = NULL;

  GList *l = g_list_first(gmic_command->parameters);
  while(l)
  {
    dt_gmic_parameter_t *param = (dt_gmic_parameter_t *)(l->data);

    if(param->id == param_id)
    {
      parameter = param;
      break;
    }

    l = g_list_next(l);
  }

  return parameter;
}

// set iop_value (this module parameter) based on the default values of gmic_parameter (the parameter from GMIC command)
static gboolean _set_iop_gmic_dt_command_parameter_from_gmic_parameter(dt_iop_gmic_dt_command_parameter_t *iop_value, const dt_gmic_parameter_t *parameter)
{
  gboolean param_set = TRUE;

  iop_value->id = parameter->id;
  iop_value->type = parameter->type;

  if(parameter->type == DT_GMIC_PARAM_FLOAT)
  {
    iop_value->value._float = parameter->value._float.default_value;
  }
  else if(parameter->type == DT_GMIC_PARAM_INT)
  {
    iop_value->value._int = parameter->value._int.default_value;
  }
  else if(parameter->type == DT_GMIC_PARAM_BOOL)
  {
    iop_value->value._bool = parameter->value._bool.default_value;
  }
  else if(parameter->type == DT_GMIC_PARAM_CHOICE)
  {
    iop_value->value._choice = parameter->value._choice.default_value;
  }
  else if(parameter->type == DT_GMIC_PARAM_COLOR)
  {
    iop_value->value._color.r = parameter->value._color.r;
    iop_value->value._color.g = parameter->value._color.g;
    iop_value->value._color.b = parameter->value._color.b;
  }
  else if(parameter->type == DT_GMIC_PARAM_POINT)
  {
    iop_value->value._point.x = parameter->value._point.x;
    iop_value->value._point.y = parameter->value._point.y;
  }
  else
  {
    param_set = FALSE;
    iop_value->id = 0;
    iop_value->type = DT_GMIC_PARAM_NONE;
  }

  return param_set;
}

// color picker callbacks
static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  const int which_colorpicker = g->color_picker.current_picker;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  for(int i = 0; i < g->widget_count; i++)
  {
    if(g->widgets[i].param_id > 0 && g->widgets[i].type == DT_GMIC_PARAM_COLOR)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->widgets[i].widg2),
                                   which_colorpicker == g->widgets[i].param_id);
  }

  darktable.gui->reset = reset;
}

static void _iop_color_picker_apply(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_id(g->color_picker.current_picker, p);
  if(param_index >= 0)
  {
    p->gmic_parameters[param_index].value._color.r = self->picked_color[0] * 255.f;
    p->gmic_parameters[param_index].value._color.g = self->picked_color[1] * 255.f;
    p->gmic_parameters[param_index].value._color.b = self->picked_color[2] * 255.f;

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;

    dt_iop_gmic_dt_widgets_t *gmic_dt_widget = _get_param_widget_from_id(p->gmic_parameters[param_index].id, g);
    GdkRGBA color = (GdkRGBA){
      .red = self->picked_color[0], .green = self->picked_color[1], .blue = self->picked_color[2], .alpha = 1.0
    };
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(gmic_dt_widget->widg), &color);

    darktable.gui->reset = reset;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  const int current_picker = g->color_picker.current_picker;

  for(int i = 0; i < g->widget_count; i++)
  {
    if(g->widgets[i].param_id > 0 && g->widgets[i].type == DT_GMIC_PARAM_COLOR && g->widgets[i].widg2 == button)
      g->color_picker.current_picker = g->widgets[i].param_id;
  }

  if(current_picker == g->color_picker.current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return g->color_picker.current_picker;
}

// builds and return the GMIC command ready to execute it
// returning command must be free()
static char *dt_gmic_get_command(const dt_iop_gmic_dt_params_t *p, const float zoom_scale)
{
  const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
  if(gmic_command == NULL) return NULL;

  int index = 0;
  size_t command_len = strlen(gmic_command->command);
  char *command = (char *)malloc(command_len + 1);
  strncpy(command, gmic_command->command, command_len);
  command[command_len] = 0;

  while(command[index])
  {
    // search for a parameter replacement
    while(command[index] != 0 && command[index] != '$' /*&& command[index] != '"'*/) index++;

    // found a parameter replacement
    if(command[index] == '$')
    {
      // is this a parameter?
      if(command[index + 1] >= '0' && command[index + 1] <= '9')
      {
        const int id = atoi(command + index + 1);

        dt_gmic_parameter_t *parameter = _get_parameter_by_id(gmic_command, id);
        if(parameter)
        {
          dt_iop_gmic_dt_command_parameter_t param_value = {0};

          // get the index of the parameter inside of dt_iop_gmic_dt_params_t->gmic_parameters
          const int param_index = _get_param_index_from_id(id, p);
          if(param_index >= 0)
          {
            param_value = p->gmic_parameters[param_index];
          }
          // if we don't have the parameter saved, use the default from the command
          else
          {
            _set_iop_gmic_dt_command_parameter_from_gmic_parameter(&param_value, parameter);
          }

          // go to the end of the replacement string '$i' inside the GMIC command
          int index2 = 1;
          while(command[index + index2] >= '0' && command[index + index2] <= '9') index2++;

          // set in str1 the string representing the value of the parameter so it can be used to replace in the command
          char str1[30 * 3];
          if(parameter->type == DT_GMIC_PARAM_FLOAT)
          {
            dt_ftoa(str1, param_value.value._float, sizeof(str1));
          }
          else if(parameter->type == DT_GMIC_PARAM_INT)
          {
            g_snprintf(str1, sizeof(str1), "%i", param_value.value._int);
          }
          else if(parameter->type == DT_GMIC_PARAM_BOOL)
          {
            if(param_value.value._bool)
              str1[0] = '1';
            else
              str1[0] = '0';
            str1[1] = 0;
          }
          else if(parameter->type == DT_GMIC_PARAM_CHOICE)
          {
            g_snprintf(str1, sizeof(str1), "%i", param_value.value._choice);
          }
          else if(parameter->type == DT_GMIC_PARAM_COLOR)
          {
            char str2[30];
            dt_ftoa(str2, param_value.value._color.r, sizeof(str2));
            strncpy(str1, str2, sizeof(str1));
            str1[sizeof(str1) - 1] = 0;
            dt_ftoa(str2, param_value.value._color.g, sizeof(str2));
            g_strlcat(str1, ",", sizeof(str1));
            g_strlcat(str1, str2, sizeof(str1));
            dt_ftoa(str2, param_value.value._color.b, sizeof(str2));
            g_strlcat(str1, ",", sizeof(str1));
            g_strlcat(str1, str2, sizeof(str1));
          }
          else if(parameter->type == DT_GMIC_PARAM_POINT)
          {
            char str2[30];
            dt_ftoa(str2, param_value.value._point.x, sizeof(str2));
            strncpy(str1, str2, sizeof(str1));
            str1[sizeof(str1) - 1] = 0;
            dt_ftoa(str2, param_value.value._point.y, sizeof(str2));
            g_strlcat(str1, ",", sizeof(str1));
            g_strlcat(str1, str2, sizeof(str1));
          }

          // replace the '$i' with the actual value
          const int len = strlen(str1);
          char *tmp = (char *)malloc(command_len + len + 1);

          strncpy(tmp, command, index);
          tmp[index] = 0;

          strncpy(tmp + index, str1, len);
          tmp[index + len] = 0;

          strncpy(tmp + index + len, command + index + index2, command_len - index);
          tmp[index + len + (command_len - index)] = 0;

          command_len += len;
          free(command);
          command = tmp;
        }
        else
        {
          fprintf(stderr, "[dt_gmic_get_command] invalid parameter $%i\n", id);
          break;
        }
      }
      else if(strncmp(command + index, "$DT_ZOOM_SCALE", 14) == 0)
      {
        int index2 = 14;
        char str1[30];
        dt_ftoa(str1, zoom_scale, sizeof(str1));

        const int len = strlen(str1);
        char *tmp = (char *)malloc(command_len + len + 1);

        strncpy(tmp, command, index);
        tmp[index] = 0;

        strncpy(tmp + index, str1, len);
        tmp[index + len] = 0;

        strncpy(tmp + index + len, command + index + index2, command_len - index);
        tmp[index + len + (command_len - index)] = 0;

        command_len += len;
        free(command);
        command = tmp;
      }
      else
      {
        index++;
      }
    }
  }

  return command;
}

// callback for all float sliders on a GMIC command
static void sl_float_widget_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(slider, g);
  if(param_index >= 0)
  {
    //    const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
    //    const dt_gmic_parameter_t *parameter = _get_parameter_by_id(gmic_command,
    //    p->gmic_parameters[param_index].id);
    p->gmic_parameters[param_index].value._float = dt_bauhaus_slider_get(slider);
    //    if(parameter->percent) p->gmic_parameters[param_index]._float /= 100.f;
  }

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// callback for all int sliders on a GMIC command
static void sl_int_widget_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(slider, g);
  if(param_index >= 0)
  {
    //    const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
    //    const dt_gmic_parameter_t *parameter = _get_parameter_by_id(gmic_command,
    //    p->gmic_parameters[param_index].id);
    p->gmic_parameters[param_index].value._int = dt_bauhaus_slider_get(slider);
    //    if(parameter->percent) p->gmic_parameters[param_index]._int /= 100;
  }

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// callback for all checkbox on a GMIC command
static void chk_widget_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(widget, g);
  if(param_index >= 0)
  {
    p->gmic_parameters[param_index].value._bool = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  }

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// callback for all combobox on a GMIC command
static void cmb_widget_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(widget, g);
  if(param_index >= 0)
  {
    p->gmic_parameters[param_index].value._choice = dt_bauhaus_combobox_get(widget);
  }
  else
    fprintf(stderr, "[cmb_widget_callback] invalid parameter index %i\n", param_index);

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// callback for all color buttons on a GMIC command
static void color_widget_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(GTK_WIDGET(widget), g);
  if(param_index >= 0)
  {
    GdkRGBA color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &color);

    p->gmic_parameters[param_index].value._color.r = color.red * 255.f;
    p->gmic_parameters[param_index].value._color.g = color.green * 255.f;
    p->gmic_parameters[param_index].value._color.b = color.blue * 255.f;
  }
  else
    fprintf(stderr, "[color_widget_callback] invalid parameter index %i\n", param_index);

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// callback for all points on a GMIC command
static void point_widget_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const int param_index = _get_param_index_from_widget(GTK_WIDGET(widget), g);
  if(param_index >= 0)
  {
    dt_iop_gmic_dt_widgets_t *gmic_dt_widget = _get_param_widget_from_id(p->gmic_parameters[param_index].id, g);

    if(gmic_dt_widget->widg == widget)
      p->gmic_parameters[param_index].value._point.x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    else if(gmic_dt_widget->widg2 == widget)
      p->gmic_parameters[param_index].value._point.y = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
  }
  else
    fprintf(stderr, "[point_widget_callback] invalid parameter index %i\n", param_index);

  dt_iop_color_picker_reset(self, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// update the values of all widgets on current GMIC command
static void _update_controls(dt_iop_module_t *self)
{
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
  if(gmic_command == NULL) return;

  for(int param_index = 0; param_index < DT_GMIC_PARAMETERS_LEN; param_index++)
  {
    if(p->gmic_parameters[param_index].id > 0)
    {
      const dt_gmic_parameter_t *parameter
          = _get_parameter_by_id(gmic_command, p->gmic_parameters[param_index].id);
      if(parameter == NULL)
      {
        fprintf(stderr, "[_update_controls] parameter %i do not exists\n", p->gmic_parameters[param_index].id);
        continue;
      }

      dt_iop_gmic_dt_widgets_t *gmic_dt_widget = _get_param_widget_from_id(p->gmic_parameters[param_index].id, g);

      if(parameter->type == DT_GMIC_PARAM_FLOAT)
      {
        dt_bauhaus_slider_set(gmic_dt_widget->widg, p->gmic_parameters[param_index].value._float);
      }
      else if(parameter->type == DT_GMIC_PARAM_INT)
      {
        dt_bauhaus_slider_set(gmic_dt_widget->widg, p->gmic_parameters[param_index].value._int);
      }
      else if(parameter->type == DT_GMIC_PARAM_BOOL)
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gmic_dt_widget->widg), p->gmic_parameters[param_index].value._bool);
      }
      else if(parameter->type == DT_GMIC_PARAM_CHOICE)
      {
        dt_bauhaus_combobox_set(gmic_dt_widget->widg, p->gmic_parameters[param_index].value._choice);
      }
      else if(parameter->type == DT_GMIC_PARAM_COLOR)
      {
        GdkRGBA color = (GdkRGBA){ .red = p->gmic_parameters[param_index].value._color.r / 255.f,
                                   .green = p->gmic_parameters[param_index].value._color.g / 255.f,
                                   .blue = p->gmic_parameters[param_index].value._color.b / 255.f,
                                   .alpha = 1.0 };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(gmic_dt_widget->widg), &color);
      }
      else if(parameter->type == DT_GMIC_PARAM_POINT)
      {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widget->widg), p->gmic_parameters[param_index].value._point.x);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widget->widg2), p->gmic_parameters[param_index].value._point.y);
      }
    }
  }
}

static void _add_widget_to_list(dt_iop_gmic_dt_widgets_t *gmic_dt_widget, dt_iop_gmic_dt_gui_data_t *g)
{
  if(gmic_dt_widget->widg)
  {
    int index = 0;
    if(g->widgets == NULL)
    {
      g->widget_count = 10;
      g->widgets = calloc(g->widget_count, sizeof(dt_iop_gmic_dt_widgets_t));
    }
    else
    {
      while(index < g->widget_count && g->widgets[index].param_id > 0) index++;
      if(index >= g->widget_count)
      {
        g->widgets = realloc(g->widgets, (g->widget_count + 10) * sizeof(dt_iop_gmic_dt_widgets_t));
        for(int i = g->widget_count; i < g->widget_count + 10; i++)
        {
          g->widgets[i].param_id = 0;
          g->widgets[i].type = DT_GMIC_PARAM_NONE;
          g->widgets[i].widg = g->widgets[i].widg2 = NULL;
        }

        index = g->widget_count;
        g->widget_count += 10;
      }
    }
    if(index >= 0)
    {
      g->widgets[index] = *gmic_dt_widget;
    }
  }
}

// create all widgets for the current GMIC command
// it destroys all currently created widgets
static void _create_command_controls(dt_iop_module_t *self)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  dt_iop_color_picker_reset(self, TRUE);

  gboolean colorbutton_created = FALSE;

  if(g->vbox_gmic_params)
  {
    for(int i = 0; i < g->widget_count; i++)
    {
      if(g->widgets[i].widg2 && GTK_IS_SPIN_BUTTON(g->widgets[i].widg2))
      {
        dt_gui_key_accel_block_on_focus_disconnect(g->widgets[i].widg2);
      }
    }

    gtk_widget_destroy(g->vbox_gmic_params);
  }

  for(int i = 0; i < g->widget_count; i++)
  {
    g->widgets[i].param_id = 0;
    g->widgets[i].type = 0;
    g->widgets[i].widg = g->widgets[i].widg2 = NULL;
  }

  g->vbox_gmic_params = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), g->vbox_gmic_params, TRUE, TRUE, 0);

  const dt_gmic_command_t *gmic_command = (p->gmic_command_name[0]) ? _get_gmic_command_by_name(p->gmic_command_name): NULL;

  if(gmic_command)
  {
    GList *l = g_list_first(gmic_command->parameters);
    while(l)
    {
      dt_gmic_parameter_t *parameter = (dt_gmic_parameter_t *)(l->data);

      dt_iop_gmic_dt_widgets_t gmic_dt_widg = {0};

      gmic_dt_widg.param_id = parameter->id;
      gmic_dt_widg.type = parameter->type;

      if(parameter->type == DT_GMIC_PARAM_FLOAT)
      {
        gmic_dt_widg.widg = dt_bauhaus_slider_new_with_range(
            self, parameter->value._float.min_value, parameter->value._float.max_value,
            parameter->value._float.increment, parameter->value._float.default_value,
            parameter->value._float.num_decimals);
        dt_bauhaus_widget_set_label(gmic_dt_widg.widg, NULL, parameter->description);
        if(parameter->percent)
        {
          char format[31];
          snprintf(format, sizeof(format), "%%.0%if%%%%", parameter->value._float.num_decimals);
          dt_bauhaus_slider_set_format(gmic_dt_widg.widg, format);
        }
        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "value-changed", G_CALLBACK(sl_float_widget_callback), self);
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, TRUE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
      }
      else if(parameter->type == DT_GMIC_PARAM_INT)
      {
        gmic_dt_widg.widg = dt_bauhaus_slider_new_with_range(self, parameter->value._int.min_value,
                                                parameter->value._int.max_value, parameter->value._int.increment,
                                                parameter->value._int.default_value, 0);
        dt_bauhaus_widget_set_label(gmic_dt_widg.widg, NULL, parameter->description);
        if(parameter->percent)
        {
          dt_bauhaus_slider_set_format(gmic_dt_widg.widg, "%i%%");
        }
        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "value-changed", G_CALLBACK(sl_int_widget_callback), self);
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, TRUE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
      }
      else if(parameter->type == DT_GMIC_PARAM_BOOL)
      {
        gmic_dt_widg.widg = gtk_check_button_new_with_label(parameter->description);

        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "toggled", G_CALLBACK(chk_widget_callback), self);
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, TRUE, TRUE, 0);

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gmic_dt_widg.widg), parameter->value._bool.default_value);

        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
      }
      else if(parameter->type == DT_GMIC_PARAM_CHOICE)
      {
        gmic_dt_widg.widg = dt_bauhaus_combobox_new(self);
        dt_bauhaus_widget_set_label(gmic_dt_widg.widg, NULL, parameter->description);
        GList *ll = g_list_first(parameter->value._choice.list_values);
        while(ll)
        {
          char *text = (char *)ll->data;

          dt_bauhaus_combobox_add(gmic_dt_widg.widg, text);

          ll = g_list_next(ll);
        }

        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "value-changed", G_CALLBACK(cmb_widget_callback), self);
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, TRUE, TRUE, 0);

        dt_bauhaus_combobox_set(gmic_dt_widg.widg, parameter->value._choice.default_value);

        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
      }
      else if(parameter->type == DT_GMIC_PARAM_COLOR)
      {
        colorbutton_created = TRUE;

        GtkWidget *hbox_color_pick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        GtkWidget *label = gtk_label_new(parameter->description);
        gtk_box_pack_start(GTK_BOX(hbox_color_pick), label, FALSE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(label));

        GdkRGBA color = (GdkRGBA){ .red = parameter->value._color.r / 255.f,
                                   .green = parameter->value._color.g / 255.f,
                                   .blue = parameter->value._color.b / 255.f,
                                   .alpha = 1.0 };

        gmic_dt_widg.widg2 = GTK_WIDGET(
            dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL));
        gtk_widget_set_size_request(GTK_WIDGET(gmic_dt_widg.widg2), DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
        g_signal_connect(G_OBJECT(gmic_dt_widg.widg2), "toggled", G_CALLBACK(dt_iop_color_picker_callback), &g->color_picker);
        gtk_box_pack_end(GTK_BOX(hbox_color_pick), gmic_dt_widg.widg2, FALSE, FALSE, 0);

        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg2));

        gmic_dt_widg.widg = gtk_color_button_new_with_rgba(&color);

        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "color-set", G_CALLBACK(color_widget_callback), self);

        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));

        gtk_box_pack_end(GTK_BOX(hbox_color_pick), gmic_dt_widg.widg, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), GTK_WIDGET(hbox_color_pick), TRUE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(hbox_color_pick));
      }
      else if(parameter->type == DT_GMIC_PARAM_POINT)
      {
        GtkWidget *hbox_point = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        GtkWidget *label = gtk_label_new(parameter->description);
        gtk_box_pack_start(GTK_BOX(hbox_point), label, FALSE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(label));

        // the color patch
        GdkRGBA color = (GdkRGBA){ .red = parameter->value._point.r / 255.f,
                                   .green = parameter->value._point.g / 255.f,
                                   .blue = parameter->value._point.b / 255.f,
                                   .alpha = parameter->value._point.a / 255.f };

        // labels and x, y controls
        gmic_dt_widg.widg2 = gtk_spin_button_new_with_range(-200, 200, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(gmic_dt_widg.widg2), 2);
        dt_gui_key_accel_block_on_focus_connect(gmic_dt_widg.widg2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widg.widg2), parameter->value._point.y);
        g_signal_connect(G_OBJECT(gmic_dt_widg.widg2), "value-changed", G_CALLBACK(point_widget_callback), self);
        gtk_box_pack_end(GTK_BOX(hbox_point), gmic_dt_widg.widg2, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg2));

        label = gtk_label_new(_("y"));
        gtk_box_pack_end(GTK_BOX(hbox_point), label, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(label));

        gmic_dt_widg.widg = gtk_spin_button_new_with_range(-200, 200, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(gmic_dt_widg.widg), 2);
        dt_gui_key_accel_block_on_focus_connect(gmic_dt_widg.widg);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widg.widg), parameter->value._point.x);
        g_signal_connect(G_OBJECT(gmic_dt_widg.widg), "value-changed", G_CALLBACK(point_widget_callback), self);
        gtk_box_pack_end(GTK_BOX(hbox_point), gmic_dt_widg.widg, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));

        label = gtk_label_new(_("x"));
        gtk_box_pack_end(GTK_BOX(hbox_point), label, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(label));

        // FIXME: there should be a better widget than a button for this
        GtkWidget *color_widg = gtk_color_button_new_with_rgba(&color);
        gtk_widget_set_sensitive(color_widg, FALSE);
        gtk_box_pack_end(GTK_BOX(hbox_point), color_widg, FALSE, FALSE, 0);
        gtk_widget_show(color_widg);

        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), GTK_WIDGET(hbox_point), TRUE, TRUE, 0);
        gtk_widget_show(GTK_WIDGET(hbox_point));
      }
      else if(parameter->type == DT_GMIC_PARAM_SEPARATOR)
      {
        gmic_dt_widg.widg = dt_ui_section_label_new((parameter->value._separator) ? parameter->value._separator : "");
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
        gmic_dt_widg.widg = NULL;
      }
      else if(parameter->type == DT_GMIC_PARAM_NOTE)
      {
        gmic_dt_widg.widg = gtk_label_new(parameter->value._note);
        gtk_box_pack_start(GTK_BOX(g->vbox_gmic_params), gmic_dt_widg.widg, FALSE, FALSE, 0);
        gtk_widget_show(GTK_WIDGET(gmic_dt_widg.widg));
        gmic_dt_widg.widg = NULL;
      }

      _add_widget_to_list(&gmic_dt_widg, g);

      l = g_list_next(l);
    }
  }

  gtk_widget_show(GTK_WIDGET(g->vbox_gmic_params));

  if(colorbutton_created)
  {
    dt_iop_init_picker(&g->color_picker, self, DT_COLOR_PICKER_POINT, _iop_color_picker_get_set,
                       _iop_color_picker_apply, _iop_color_picker_update);
  }

  darktable.gui->reset = reset;
}

// sets the default values of the current GMIC command in dt_iop_gmic_dt_params_t
static void _load_command_default_values(dt_iop_gmic_dt_params_t *p)
{
  memset(&p->gmic_parameters, 0, sizeof(p->gmic_parameters));

  dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
  if(gmic_command)
  {
    int param_index = 0;

    GList *l = g_list_first(gmic_command->parameters);
    while(l)
    {
      dt_gmic_parameter_t *parameter = (dt_gmic_parameter_t *)(l->data);

      if(_set_iop_gmic_dt_command_parameter_from_gmic_parameter(p->gmic_parameters + param_index, parameter))
        param_index++;

      l = g_list_next(l);
    }
  }
}

// combo box with all GMIC commands callback
static void _gmic_commands_callback(GtkComboBox *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;

  int index = dt_bauhaus_combobox_get((GtkWidget *)combo);
  if(index >= 0)
  {
    dt_gmic_command_t *command = NULL;
    GList *l = g_list_nth(darktable.gmic_commands, index);
    if(l) command = (dt_gmic_command_t *)(l->data);
    if(command)
    {
      g_strlcpy(p->gmic_command_name, command->name, sizeof(p->gmic_command_name));
      p->colorspace = command->colorspace;
      p->scale_image = command->scale_image;
    }
  }

  _load_command_default_values(p);
  _create_command_controls(self);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// FIXME: this does not handle distortions yet!

#define DT_IOP_GMIC_POINT_RADIUS 5.f

// if x, y is over an overlay (a point() for now) it returns the index of the dt_iop_gmic_dt_params_t params
static int _hit_test(struct dt_iop_module_t *self, const dt_iop_gmic_dt_params_t *p, const dt_iop_gmic_dt_gui_data_t *g, double __x, double __y)
{
  int index = -1;
  const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
  if(gmic_command == NULL) return index;

  dt_develop_t *dev = self->dev;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;

  const float mouse_x = gui->posx;
  const float mouse_y = gui->posy;

  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  const float rad_mult = (1.f / zoom_scale);

  for(int i = 0; i < g->widget_count; i++)
  {
    if(g->widgets[i].param_id > 0 &&  g->widgets[i].type == DT_GMIC_PARAM_POINT)
    {
      const int param_index = _get_param_index_from_id(g->widgets[i].param_id, p);
      if(param_index >= 0)
      {
        const dt_gmic_parameter_t *parameter = _get_parameter_by_id(gmic_command, p->gmic_parameters[param_index].id);
        if(parameter)
        {
          const float radius = DT_IOP_GMIC_POINT_RADIUS;
          const float delta = radius * rad_mult * 1.5f;
          const float x = wd * p->gmic_parameters[param_index].value._point.x / 100.f;
          const float y = ht * p->gmic_parameters[param_index].value._point.y / 100.f;

          if(mouse_x < x + delta && mouse_x > x - delta && mouse_y < y + delta && mouse_y > y - delta)
          {
            index = param_index;
            break;
          }
        }
      }
    }
  }

  return index;
}

// GUI for point() parameters
int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int handled = 0;

  dt_develop_t *dev = self->dev;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;

  if(g->dragging_index >= 0)
  {
    p->gmic_parameters[g->dragging_index].value._point.x = (gui->posx / wd) * 100.f;
    p->gmic_parameters[g->dragging_index].value._point.y = (gui->posy / ht) * 100.f;

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;

    dt_iop_gmic_dt_widgets_t *gmic_dt_widget = _get_param_widget_from_id(p->gmic_parameters[g->dragging_index].id, g);
    if(gmic_dt_widget)
    {
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widget->widg), p->gmic_parameters[g->dragging_index].value._point.x);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(gmic_dt_widget->widg2), p->gmic_parameters[g->dragging_index].value._point.y);
    }

    darktable.gui->reset = reset;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  return handled;
}

int scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state)
{
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int handled = 0;

  return handled;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int handled = 0;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;

  if(which == 1)
  {
    g->dragging_index = _hit_test(self, p, g, x, y);
    if(g->dragging_index >= 0)
    {
      handled = 1;
    }
  }

  return handled;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int handled = 0;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  if(g->dragging_index >= 0)
  {
    handled = 1;
    g->dragging_index = -1;
  }

  return handled;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;
  if(!g) return;
  if(!g->draw_overlays) return;

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;

  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  const float rad_mult = 1.f / zoom_scale;

  double dashed[] = { 4.0, 4.0 };

  cairo_save(cr);

  cairo_translate(cr, width / 2.0, height / 2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // draw all points
  const dt_gmic_command_t *gmic_command = _get_gmic_command_by_name(p->gmic_command_name);
  if(gmic_command == NULL) goto done;

  for(int param_index = 0; param_index < DT_GMIC_PARAMETERS_LEN; param_index++)
  {
    if(p->gmic_parameters[param_index].id > 0)
    {
      const dt_gmic_parameter_t *parameter = _get_parameter_by_id(gmic_command, p->gmic_parameters[param_index].id);
      if(parameter)
      {
        if(parameter->type == DT_GMIC_PARAM_POINT)
        {
          const float xpos = wd * (p->gmic_parameters[param_index].value._point.x / 100.f);
          const float ypos = ht * (p->gmic_parameters[param_index].value._point.y / 100.f);
          const float radius = DT_IOP_GMIC_POINT_RADIUS * rad_mult;

          cairo_set_dash(cr, dashed, 0, 0);
          cairo_set_line_width(cr, 3.0 / zoom_scale);
          cairo_set_source_rgba(cr, parameter->value._point.r / 255.f, parameter->value._point.g / 255.f, parameter->value._point.b / 255.f,
                                  parameter->value._point.a / 255.f);
          cairo_arc(cr, xpos, ypos, radius, 0, 2.0 * M_PI);
          cairo_fill(cr);
        }
      }
    }
  }

done:
  cairo_restore(cr);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(!in) dt_iop_color_picker_reset(self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_gmic_dt_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_gmic_dt_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;
  dt_iop_gmic_dt_params_t *p = (dt_iop_gmic_dt_params_t *)self->params;

  int index = -1;
  int i = -1;

  GList *l = g_list_first(darktable.gmic_commands);
  while(l)
  {
    i++;
    dt_gmic_command_t *command = (dt_gmic_command_t *)(l->data);
    if(strcmp(command->name, p->gmic_command_name) == 0)
    {
      index = i;
      break;
    }

    l = g_list_next(l);
  }

  dt_bauhaus_combobox_set(g->cmb_gmic_commands, index);

  if(darktable.gui->reset) _create_command_controls(self);

  _update_controls(self);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_gmic_dt_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_gmic_dt_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_gmic_dt_params_t);
  module->gui_data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_gmic_dt_params_t tmp = { 0 };

  memcpy(module->params, &tmp, sizeof(dt_iop_gmic_dt_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_gmic_dt_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  _create_command_controls(self);

  g->draw_overlays = TRUE;
  g->dragging_index = -1;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = calloc(1, sizeof(dt_iop_gmic_dt_gui_data_t));
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  g->widget_count = 0;
  g->widgets = NULL;
  g->draw_overlays = TRUE;
  g->dragging_index = -1;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  // all gmic commands
  g->cmb_gmic_commands = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cmb_gmic_commands, NULL, _("gmic command"));

  GList *l = g_list_first(darktable.gmic_commands);
  while(l)
  {
    dt_gmic_command_t *command = (dt_gmic_command_t *)(l->data);
    dt_bauhaus_combobox_add(g->cmb_gmic_commands, command->description);

    l = g_list_next(l);
  }

  g_object_set(g->cmb_gmic_commands, "tooltip-text", _("select a gmic command"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->cmb_gmic_commands), "value-changed", G_CALLBACK(_gmic_commands_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->cmb_gmic_commands), TRUE, TRUE, 0);

  dt_iop_init_picker(&g->color_picker, self, DT_COLOR_PICKER_POINT, _iop_color_picker_get_set,
                     _iop_color_picker_apply, _iop_color_picker_update);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_gmic_dt_gui_data_t *g = (dt_iop_gmic_dt_gui_data_t *)self->gui_data;

  if(g->widgets) free(g->widgets);

  free(self->gui_data);
  self->gui_data = NULL;
}

// TODO: this is copied from pixelpipe_hb.c, should we export it?
static char *_pipe_type_to_str(int pipe_type)
{
  char *r;

  switch(pipe_type)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      r = "preview";
      break;
    case DT_DEV_PIXELPIPE_PREVIEW2:
      r = "preview2";
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = "full";
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      r = "thumbnail";
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      r = "export";
      break;
    default:
      r = "unknown";
  }
  return r;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_gmic_dt_data_t *p = (dt_iop_gmic_dt_data_t *)piece->data;

  if(p->gmic_command_name[0] == 0)
  {
    memcpy(ovoid, ivoid, roi_in->width * roi_in->height * 4 * sizeof(float));
    return;
  }

  char *command = dt_gmic_get_command(p, roi_in->scale / piece->iscale);
  if(command)
  {
    printf("\n[gmic process] processing image of with %i and height %i, image scale %f and colorspace=%i %s on pipe %s\n",
        roi_in->width, roi_in->height, roi_in->scale / piece->iscale, p->colorspace,
        (p->scale_image) ? "(scaled)": "", _pipe_type_to_str(piece->pipe->type));
    printf("\n[gmic process] command '%s'\n'%s'\n\n", p->gmic_command_name, command);

    if(p->colorspace == DT_GMIC_sRGB_3C)
    {
      const dt_iop_order_iccprofile_info_t *const srgb_profile
          = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "", INTENT_PERCEPTUAL);
      const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
      if(work_profile && srgb_profile)
      {
        dt_ioppr_transform_image_colorspace_rgb(ivoid, ovoid, roi_in->width, roi_in->height, work_profile,
                                                srgb_profile, "GMIC process");

        dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

        dt_gmic_run_3c(ovoid, ovoid, roi_in->width, roi_in->height, command, p->scale_image);

        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

        dt_ioppr_transform_image_colorspace_rgb(ovoid, ovoid, roi_in->width, roi_in->height, srgb_profile,
                                                work_profile, "GMIC process");
      }
    }
    else if(p->colorspace == DT_GMIC_sRGB_1C)
    {
      const dt_iop_order_iccprofile_info_t *const srgb_profile
          = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "", INTENT_PERCEPTUAL);
      const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
      if(work_profile && srgb_profile)
      {
        dt_ioppr_transform_image_colorspace_rgb(ivoid, ovoid, roi_in->width, roi_in->height, work_profile,
                                                srgb_profile, "GMIC process");

        dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

        dt_gmic_run_1c(ovoid, ovoid, roi_in->width, roi_in->height, command, p->scale_image);

        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

        dt_ioppr_transform_image_colorspace_rgb(ovoid, ovoid, roi_in->width, roi_in->height, srgb_profile,
                                                work_profile, "GMIC process");
      }
    }
    else if(p->colorspace == DT_GMIC_RGB_3C || p->colorspace == DT_GMIC_LAB_3C)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

      dt_gmic_run_3c(ivoid, ovoid, roi_in->width, roi_in->height, command, p->scale_image);

      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }
    else if(p->colorspace == DT_GMIC_RGB_1C || p->colorspace == DT_GMIC_LAB_1C)
    {
      memcpy(ovoid, ivoid, roi_in->width * roi_in->height * 4 * sizeof(float));

      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

      dt_gmic_run_1c(ovoid, ovoid, roi_in->width, roi_in->height, command, p->scale_image);

      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }

    free(command);

    if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
      dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }
  else
    fprintf(stderr, "[gmic process]: error generating GMIC command %s\n", p->gmic_command_name);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;

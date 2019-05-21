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

#define __STDC_FORMAT_MACROS
#define cimg_OS 1
#include <gmic.h>
extern "C"
{
#include "common/gmic_dt.h"
#include "control/conf.h"
#include <stdlib.h>

GList *_parse_gmic_commands(const char *commands)
{
  GList *temp_commands = NULL;

  return temp_commands;
}

static void _gmic_parameter_free(void *_gmic_parameter)
{
  dt_gmic_parameter_t *parameter = (dt_gmic_parameter_t *)_gmic_parameter;

  if(parameter->type == DT_GMIC_PARAM_CHOICE)
  {
    if(parameter->value._choice.list_values) g_list_free_full(parameter->value._choice.list_values, free);
  }
  else if(parameter->type == DT_GMIC_PARAM_SEPARATOR)
  {
    if(parameter->value._separator) free(parameter->value._separator);
  }
  else if(parameter->type == DT_GMIC_PARAM_NOTE)
  {
    if(parameter->value._note) free(parameter->value._note);
  }

  free(parameter);
}

static void _gmic_command_free(void *_gmic_command)
{
  dt_gmic_command_t *gmic_command = (dt_gmic_command_t *)_gmic_command;
  if(gmic_command)
  {
    if(gmic_command->parameters) g_list_free_full(gmic_command->parameters, _gmic_parameter_free);
    if(gmic_command->command) free(gmic_command->command);
    free(gmic_command);
  }
}

void dt_gmic_commands_cleanup()
{
  if(darktable.gmic_commands) g_list_free_full(darktable.gmic_commands, _gmic_command_free);
  if(darktable.gmic_custom_commands) free(darktable.gmic_custom_commands);
}

// str -->float, with decimal separator == '.'
inline float dt_atof(const char *const str)
{
  float val_f = 0.f;
  int val1 = 0;
  int val2 = 0;
  sscanf(str, "%i.%i", &val1, &val2);
  val_f = (float)val2;

  while(val_f >= 1.f)
  {
    val_f = val_f / 10.f;
  }
  val_f = val_f + (float)val1;

  return val_f;
}

#define DT_GMIC_MAX_LINE_LEN 255
#define DT_GMIC_MAX_WORD_LEN 45

typedef enum dt_gmic_parser_word_type_t
{
  DT_GMIC_PARSER_WORD_INTEGER = 1,
  DT_GMIC_PARSER_WORD_FLOAT = 2,
  DT_GMIC_PARSER_WORD_ALPHA = 3,
  DT_GMIC_PARSER_WORD_CHAR = 5
} dt_gmic_parser_word_type_t;

typedef struct dt_gmic_parser_t
{
  FILE *pFile;
  int index;
  int current_word;
  int line_number;
  gboolean done;
  gboolean error;
  char buffer[DT_GMIC_MAX_LINE_LEN + 1];
  char word[DT_GMIC_MAX_WORD_LEN + 1];
  dt_gmic_parser_word_type_t word_type;
  dt_gmic_command_t *gmic_command;
} dt_gmic_parser_t;

static inline void _parser_skip_blanks(dt_gmic_parser_t *parser)
{
  while(parser->buffer[parser->index] == ' ' || parser->buffer[parser->index] == '\t'
        || parser->buffer[parser->index] == '\n')
    parser->index++;
}

static inline void _parser_trim_blanks(char *text)
{
  int index2 = strlen(text) - 1;
  while(index2 >= 0 && (text[index2] == ' ' || text[index2] == '\t' || text[index2] == '\n')) index2--;
  text[index2 + 1] = 0;
}

static void _parser_print_error(dt_gmic_parser_t *parser, const char *err_msg)
{
  parser->error = parser->done = TRUE;

  fprintf(stderr, "[dt_load_gmic_commands_from_file] %s line %i (%i)\n", err_msg, parser->line_number,
          parser->index + 1);
  const int len = strlen(parser->buffer);
  if(len > 0 && parser->buffer[len - 1] == '\n')
    fprintf(stderr, "%s", parser->buffer);
  else
    fprintf(stderr, "%s\n", parser->buffer);
  if(parser->index >= 0 && parser->index < (int)sizeof(parser->buffer) - 1)
  {
    char text[sizeof(parser->buffer)];
    for(int i = 0; i < (int)sizeof(parser->buffer); i++) text[i] = ' ';
    text[sizeof(text) - 1] = 0;
    text[parser->index] = '^';
    fprintf(stderr, "%s\n", text);
  }
}

static void _parser_read_line(dt_gmic_parser_t *parser)
{
  gboolean line_ok = FALSE;

  while(!line_ok && !parser->done)
  {
    parser->index = 0;
    parser->current_word = 0;
    parser->line_number++;
    parser->word[0] = 0;
    parser->done = (fgets(parser->buffer, DT_GMIC_MAX_LINE_LEN, parser->pFile) == NULL);

    line_ok = TRUE;
    if(!parser->done) _parser_skip_blanks(parser);

    if(!parser->done)
    {
      // skip comments
      if(parser->buffer[parser->index] == '#' && parser->buffer[parser->index + 1] != '@')
      {
        line_ok = FALSE;
      }
      // skip empty lines
      else if(parser->buffer[parser->index] == 0)
      {
        line_ok = FALSE;
      }
    }
  }
}

static inline gboolean _is_alpha(const char *text)
{
  return ((*text >= 'A' && *text <= 'Z') || (*text >= 'a' && *text <= 'z'));
}

static inline gboolean _is_digit(const char *text)
{
  return (*text >= '0' && *text <= '9');
}

static gboolean _parser_read_one_word(dt_gmic_parser_t *parser)
{
  gboolean word_readed = FALSE;

  parser->current_word = parser->index;
  _parser_skip_blanks(parser);

  if(parser->buffer[parser->index] == '#')
  {
    if(parser->buffer[parser->index + 1] == '@')
    {
      parser->index += 2;
      while(_is_alpha(parser->buffer + parser->index)) parser->index++;
      const size_t len = parser->index - parser->current_word;
      if(len < sizeof(parser->word))
      {
        strncpy(parser->word, parser->buffer + parser->current_word, len);
        parser->word[len] = 0;
        parser->word_type = DT_GMIC_PARSER_WORD_ALPHA;
        word_readed = TRUE;
      }
      else
      {
        _parser_print_error(parser, "error: unknown word");
      }
    }
  }
  // an alphanumeric word
  else if(_is_alpha(parser->buffer + parser->index) || parser->buffer[parser->index] == '_')
  {
    parser->index++;

    while(_is_alpha(parser->buffer + parser->index) || _is_digit(parser->buffer + parser->index)
          || parser->buffer[parser->index] == '_')
      parser->index++;
    const size_t len = parser->index - parser->current_word;
    if(len < sizeof(parser->word))
    {
      strncpy(parser->word, parser->buffer + parser->current_word, len);
      parser->word[len] = 0;
      parser->word_type = DT_GMIC_PARSER_WORD_ALPHA;
      word_readed = TRUE;
    }
    else
    {
      _parser_print_error(parser, "error: unknown word");
    }
  }
  // a number
  else if(_is_digit(parser->buffer + parser->index)
          || ((parser->buffer[parser->index] == '-' || parser->buffer[parser->index] == '.')
              && _is_digit(parser->buffer + parser->index + 1))
          || (parser->buffer[parser->index] == '-' && parser->buffer[parser->index + 1] == '.'
              && _is_digit(parser->buffer + parser->index + 2)))
  {
    parser->index++;

    // the integer part
    while(_is_digit(parser->buffer + parser->index)) parser->index++;
    parser->word_type = DT_GMIC_PARSER_WORD_INTEGER;
    // the decimal part
    if(parser->buffer[parser->index] == '.')
    {
      parser->word_type = DT_GMIC_PARSER_WORD_FLOAT;
      parser->index++;
      while(_is_digit(parser->buffer + parser->index)) parser->index++;
    }
    const size_t len = parser->index - parser->current_word;
    if(len < sizeof(parser->word))
    {
      strncpy(parser->word, parser->buffer + parser->current_word, len);
      parser->word[len] = 0;
      word_readed = TRUE;
    }
    else
    {
      _parser_print_error(parser, "error: unknown word");
    }
  }
  // a character
  else
  {
    parser->word[0] = parser->buffer[parser->index];
    parser->word[1] = 0;
    parser->index++;
    word_readed = TRUE;
    parser->word_type = DT_GMIC_PARSER_WORD_CHAR;
  }

  return word_readed;
}

static void _parser_read_word(dt_gmic_parser_t *parser)
{
  gboolean word_readed = FALSE;

  if(!parser->done) _parser_skip_blanks(parser);

  if(!parser->done)
  {
    if(parser->buffer[parser->index] == 0) _parser_read_line(parser);
  }

  while(!word_readed && !parser->done)
  {
    word_readed = _parser_read_one_word(parser);
    if(!word_readed) _parser_read_line(parser);
  }
}

static char *_parser_read_parameter_string(dt_gmic_parser_t *parser)
{
  char *text = NULL;

  if(parser->word[0] == '"' || parser->word[0] == '\'')
  {
    int text_len = 0;
    gboolean done = FALSE;
    char delimiter = parser->word[0];
    parser->current_word++;

    while(!done && !parser->done)
    {
      while(parser->buffer[parser->index] != 0 && parser->buffer[parser->index] != delimiter) parser->index++;

      const int len = (parser->index - parser->current_word);
      if(len > 0)
      {
        if(text == NULL)
        {
          text = (char *)malloc(len + 1);
          strncpy(text, parser->buffer + parser->current_word, len);
          text[len] = 0;
          text_len = len;
        }
        else
        {
          text = (char *)realloc(text, text_len + len + 1);
          g_strlcpy(text + text_len, parser->buffer + parser->current_word, len + 1);
          text_len += len;
        }
      }

      if(parser->buffer[parser->index] == delimiter)
      {
        done = TRUE;
      }
      else
      {
        _parser_read_word(parser);

        // do we read a new line?
        if(!parser->done)
        {
          if(strcmp(parser->word, "#@gui") == 0)
          {
            _parser_read_word(parser);

            if(!parser->done)
            {
              if(strcmp(parser->word, ":") == 0)
              {
                parser->current_word++;
                if(parser->buffer[parser->current_word] == ' ') parser->current_word++;
              }
              else
                _parser_print_error(parser, "error: ':' expected");
            }
          }
        }
      }
    }

    if(parser->buffer[parser->index] == delimiter)
    {
      parser->index++;
      _parser_read_word(parser);
    }
    else
      _parser_print_error(parser, "error: '\" | '' expected");
  }

  return text;
}

// #@gui : {parameter id} : {parameter description} {(%)} = {parameter definition}
static void _parser_read_parameter(dt_gmic_parser_t *parser)
{
  dt_gmic_parameter_t *param = (dt_gmic_parameter_t *)calloc(1, sizeof(dt_gmic_parameter_t));

  if(strcmp(parser->word, "#@gui") == 0)
  {
    _parser_read_word(parser);
  }
  else
  {
    _parser_print_error(parser, "error: '#@gui' expected");
  }

  if(!parser->done)
  {
    if(strcmp(parser->word, ":") == 0)
    {
      _parser_read_word(parser);
    }
    else
    {
      _parser_print_error(parser, "error: ':' expected");
    }
  }

  // parameter id
  if(!parser->done)
  {
    if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
    {
      param->id = atoi(parser->word);
      _parser_read_word(parser);
    }
    else
    {
      _parser_print_error(parser, "error: parameter id expected");
    }
  }

  if(!parser->done)
  {
    if(strcmp(parser->word, ":") == 0)
    {
      _parser_read_word(parser);
    }
    else
    {
      _parser_print_error(parser, "error: ':' expected");
    }
  }

  // parameter description
  if(!parser->error)
  {
    parser->index = parser->current_word;
    while(parser->buffer[parser->index] != 0 && parser->buffer[parser->index] != '='
          && strncmp(parser->buffer + parser->index, "(%)", 3) != 0)
      parser->index++;

    g_strlcpy(param->description, parser->buffer + parser->current_word,
              MIN(sizeof(param->description), (parser->index - parser->current_word)));
    _parser_trim_blanks(param->description);
    _parser_read_word(parser);
  }

  // is this value a percent?
  if(!parser->done)
  {
    if(strcmp(parser->word, "(") == 0 && strncmp(parser->buffer + parser->index, "%)", 2) == 0)
    {
      param->percent = TRUE;
      parser->index += 2;
      _parser_read_word(parser);
    }
  }

  // we must be at the definition of the parameter
  if(!parser->done)
  {
    if(strcmp(parser->word, "=") == 0)
    {
      _parser_read_word(parser);
    }
    else
    {
      _parser_print_error(parser, "error: '=' expected");
    }
  }

  // parameter definition
  if(!parser->done)
  {
    char delimiter[2] = { 0 };
    char param_type[sizeof(parser->word)];
    g_strlcpy(param_type, parser->word, sizeof(param_type));

    _parser_read_word(parser);

    if(!parser->done)
    {
      if(strcmp(parser->word, "(") == 0 || strcmp(parser->word, "{") == 0 || strcmp(parser->word, "[") == 0)
      {
        if(parser->word[0] == '(')
          delimiter[0] = ')';
        else if(parser->word[0] == '{')
          delimiter[0] = '}';
        else
          delimiter[0] = ']';
        delimiter[1] = 0;
        _parser_read_word(parser);
      }
      else
      {
        _parser_print_error(parser, "error: '( | { | [' expected");
      }
    }

    // float(default value, min value, max value, increment, number of decimals)
    if(strcmp(param_type, "float") == 0)
    {
      param->type = DT_GMIC_PARAM_FLOAT;

      // default value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._float.default_value = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: default value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // min value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._float.min_value = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: min value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // max value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._float.max_value = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: max value expected");
      }

      // increment is optional
      param->value._float.increment = (param->value._float.max_value - param->value._float.min_value) / 10.f;
      if(param->value._float.increment <= 0.f) param->value._float.increment = 0.01;

      // increment
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._float.increment = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: increment value expected");
          }
        }
      }

      // number of decimals
      param->value._float.num_decimals = 2;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
            {
              param->value._float.num_decimals = atoi(parser->word);
              _parser_read_word(parser);
            }
            else
            {
              _parser_print_error(parser, "error: number of decimals expected");
            }
          }
        }
      }
    }
    // int: (default value, min value, max value, increment)
    else if(strcmp(param_type, "int") == 0)
    {
      param->type = DT_GMIC_PARAM_INT;

      // default value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
        {
          param->value._int.default_value = atoi(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: default value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // min value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
        {
          param->value._int.min_value = atoi(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: min value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // max value
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
        {
          param->value._int.max_value = atoi(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: max value expected");
      }

      // increment is not mandatory
      param->value._int.increment = (int)((param->value._int.max_value - param->value._int.min_value) / 10);
      if(param->value._int.increment <= 0) param->value._int.increment = 1;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          // increment
          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
            {
              param->value._int.increment = atoi(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: increment value expected");
          }
        }
      }
    }
    // bool: (0 | 1)
    else if(strcmp(param_type, "bool") == 0)
    {
      param->type = DT_GMIC_PARAM_BOOL;

      if(strcmp(parser->word, delimiter) != 0)
      {
        if(strcmp(parser->word, "1") == 0 || strcmp(parser->word, "true") == 0)
        {
          param->value._bool.default_value = TRUE;
          _parser_read_word(parser);
        }
        else if(strcmp(parser->word, "0") == 0 || strcmp(parser->word, "false") == 0)
        {
          param->value._bool.default_value = FALSE;
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: '1 | true | 0 | false' expected");
      }
    }
    // color: (red,gree,blue,alpha)
    else if(strcmp(param_type, "color") == 0)
    {
      param->type = DT_GMIC_PARAM_COLOR;

      // red
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._color.r = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: red value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // green
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._color.g = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: green value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // blue
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._color.b = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: blue value expected");
      }

      // alpha is optional
      param->value._color.a = 1.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._color.a = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: alpha value expected");
          }
        }
      }
    }
    // point: (X,Y,_removable={ -1 | 0 | 1 },_burst={ 0 | 1 },_R,_G,_B,_[-]A,_radius%)
    else if(strcmp(param_type, "point") == 0)
    {
      param->type = DT_GMIC_PARAM_POINT;

      // X
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._point.x = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: X value expected");
      }
      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
          _parser_read_word(parser);
        else
          _parser_print_error(parser, "error: ',' expected");
      }

      // Y
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
        {
          param->value._point.y = dt_atof(parser->word);
          _parser_read_word(parser);
        }
        else
          _parser_print_error(parser, "error: Y value expected");
      }

      // the following are optional
      param->value._point.removable = 0;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
            {
              param->value._point.removable = atoi(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: removable value expected");
          }
        }
      }

      param->value._point.burst = 0;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
            {
              param->value._point.burst = atoi(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: burst value expected");
          }
        }
      }

      param->value._point.r = 255.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._point.r = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: red value expected");
          }
        }
      }

      param->value._point.g = 255.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._point.g = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: green value expected");
          }
        }
      }

      param->value._point.b = 255.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._point.b = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: blue value expected");
          }
        }
      }

      param->value._point.a = 255.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._point.a = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: alpha value expected");
          }
        }
      }

      param->value._point.radius = 0.f;

      if(!parser->done)
      {
        if(strcmp(parser->word, ",") == 0)
        {
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER || parser->word_type == DT_GMIC_PARSER_WORD_FLOAT)
            {
              param->value._point.radius = dt_atof(parser->word);
              _parser_read_word(parser);
            }
            else
              _parser_print_error(parser, "error: radius value expected");
          }
        }
      }
    }
    // choice: (defalt index value, string 1, .., string n)
    else if(strcmp(param_type, "choice") == 0)
    {
      param->type = DT_GMIC_PARAM_CHOICE;

      // default index
      param->value._choice.default_value = 0;
      if(!parser->done)
      {
        if(parser->word_type == DT_GMIC_PARSER_WORD_INTEGER)
        {
          param->value._choice.default_value = atoi(parser->word);
          _parser_read_word(parser);

          if(!parser->done)
          {
            if(strcmp(parser->word, ",") == 0)
              _parser_read_word(parser);
            else
              _parser_print_error(parser, "error: ',' expected");
          }
        }
      }

      // list of strings
      if(!parser->done)
      {
        char *text = _parser_read_parameter_string(parser);

        if(text == NULL) _parser_print_error(parser, "error: choice list expected");

        while(text && !parser->done)
        {
          param->value._choice.list_values = g_list_append(param->value._choice.list_values, text);
          text = NULL;

          if(!parser->done)
          {
            if(strcmp(parser->word, ",") == 0)
            {
              _parser_read_word(parser);

              // do we read a new line?
              if(!parser->done)
              {
                if(strcmp(parser->word, "#@gui") == 0)
                {
                  _parser_read_word(parser);

                  if(!parser->done)
                  {
                    if(strcmp(parser->word, ":") == 0) _parser_read_word(parser);
                  }
                }
              }

              if(!parser->done) text = _parser_read_parameter_string(parser);
            }
          }
        }
      }
    }
    // separator: (description)
    else if(strcmp(param_type, "separator") == 0)
    {
      param->type = DT_GMIC_PARAM_SEPARATOR;

      param->value._separator = _parser_read_parameter_string(parser);
    }
    // note: (description)
    else if(strcmp(param_type, "note") == 0)
    {
      param->type = DT_GMIC_PARAM_NOTE;

      param->value._note = _parser_read_parameter_string(parser);

      if(param->value._note == NULL) _parser_print_error(parser, "error: note text expected");
    }
    else
    {
      parser->error = parser->done = TRUE;
      _parser_print_error(parser, "error: unknown parameter type");
    }

    if(!parser->done)
    {
      if(strcmp(parser->word, delimiter) == 0)
        _parser_read_word(parser);
      else
        _parser_print_error(parser, "error: ') | } | ]' expected");
    }
  }

  if(!parser->error)
  {
    parser->gmic_command->parameters = g_list_append(parser->gmic_command->parameters, param);
  }
  else
  {
    free(param);
  }
}

// read all parameter entries
static void _parser_read_parameters(dt_gmic_parser_t *parser)
{
  while(!parser->done && strcmp(parser->word, "#@gui") == 0)
  {
    _parser_read_parameter(parser);
  }
}

// #@gui {command description} : {command name}
static void _parser_read_header(dt_gmic_parser_t *parser)
{
  if(strcmp(parser->word, "#@gui") != 0)
  {
    _parser_print_error(parser, "error: '#@gui' expected");
  }

  // command description
  if(!parser->done)
  {
    parser->current_word = parser->index;

    while(parser->buffer[parser->index] != 0 && parser->buffer[parser->index] != ':') parser->index++;

    const int len = MIN(parser->index - parser->current_word, sizeof(parser->gmic_command->description));
    strncpy(parser->gmic_command->description, parser->buffer + parser->current_word, len);
    parser->gmic_command->description[sizeof(parser->gmic_command->description) - 1] = 0;

    _parser_read_word(parser);
  }

  if(!parser->done)
  {
    if(strcmp(parser->word, ":") == 0)
    {
      _parser_read_word(parser);
    }
    else
    {
      _parser_print_error(parser, "error: ':' expected");
    }
  }

  // command name
  if(!parser->done)
  {
    g_strlcpy(parser->gmic_command->name, parser->word, sizeof(parser->gmic_command->name));
    _parser_read_word(parser);
  }
}

// #@dt : { reserved word } = { reverved value }
static void _parser_read_dt_entry(dt_gmic_parser_t *parser)
{
  while(!parser->done && strcmp(parser->word, "#@dt") == 0)
  {
    char str1[sizeof(parser->word)] = { 0 };
    char str2[sizeof(parser->word)] = { 0 };

    _parser_read_word(parser);

    if(!parser->done)
    {
      if(strcmp(parser->word, ":") == 0)
        _parser_read_word(parser);
      else
        _parser_print_error(parser, "error: ':' expected");
    }

    if(!parser->done)
    {
      g_strlcpy(str1, parser->word, sizeof(str1));
      _parser_read_word(parser);
    }

    if(!parser->done)
    {
      if(strcmp(parser->word, "=") == 0)
        _parser_read_word(parser);
      else
        _parser_print_error(parser, "error: '=' expected");
    }

    if(!parser->done) g_strlcpy(str2, parser->word, sizeof(str2));

    // set the dt_entry value
    if(!parser->done)
    {
      if(strcmp(str1, "colorspace") == 0)
      {
        if(strcmp(str2, "RGB_3C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_RGB_3C;
        else if(strcmp(str2, "RGB_1C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_RGB_1C;
        else if(strcmp(str2, "sRGB_3C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_sRGB_3C;
        else if(strcmp(str2, "sRGB_1C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_sRGB_1C;
        else if(strcmp(str2, "LAB_3C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_LAB_3C;
        else if(strcmp(str2, "LAB_1C") == 0)
          parser->gmic_command->colorspace = DT_GMIC_LAB_1C;
        else
          _parser_print_error(parser, "unknown colorspace");
      }
      else if(strcmp(str1, "scale_image") == 0)
      {
        if(strcmp(str2, "true") == 0) parser->gmic_command->scale_image = TRUE;
      }
      else
        _parser_print_error(parser, "unknown dt_entry entry");
    }

    if(!parser->done) _parser_read_word(parser);
  }
}

// { command header } :
// GMIC command
static void _parser_read_command(dt_gmic_parser_t *parser)
{
  // command header must be the same as the command name already stored
  if(strcmp(parser->word, parser->gmic_command->name) == 0)
    _parser_read_word(parser);
  else
    _parser_print_error(parser, "error: command header expected");

  if(!parser->done)
  {
    if(strcmp(parser->word, ":") != 0)
      //        _parser_read_word(parser);
      //     else
      _parser_print_error(parser, "error: ':' expected");
  }

  // read all command lines
  if(!parser->done) _parser_read_line(parser);

  int command_len = 0;
  while(!parser->done && strncmp(parser->buffer + parser->index, "#@gui", 5) != 0)
  {
    int len = strlen(parser->buffer + parser->index);

    // make room for the new comman line
    if(parser->gmic_command->command)
    {
      parser->gmic_command->command = (char *)realloc(parser->gmic_command->command, command_len + len);
      g_strlcat(parser->gmic_command->command, parser->buffer + parser->index, command_len + len);
    }
    else
    {
      len++;
      parser->gmic_command->command = (char *)calloc(1, len);
      strncpy(parser->gmic_command->command, parser->buffer + parser->index, len);
    }
    command_len += len;

    if(!parser->done) _parser_read_line(parser);
  }

  // trim the command
  if(!parser->error)
  {
    command_len = strlen(parser->gmic_command->command) - 1;
    while(command_len > 0
          && (parser->gmic_command->command[command_len] == '\n'
              || parser->gmic_command->command[command_len] == ' '
              || parser->gmic_command->command[command_len] == '\t'))
      parser->gmic_command->command[command_len--] = 0;
  }

  if(!parser->done) _parser_read_word(parser);
}

// #@gui : {parameter id} : {parameter description} {(%)}  = {parameter definition}
// load commands from a single .gmic file
static GList *_load_gmic_commands_from_file(const char *gmic_file)
{
  GList *command_list = NULL;
  dt_gmic_parser_t parser = { 0 };

  parser.pFile = fopen(gmic_file, "r");
  if(parser.pFile)
  {
    _parser_read_word(&parser);
    while(!parser.done)
    {
      parser.gmic_command = (dt_gmic_command_t *)calloc(1, sizeof(dt_gmic_command_t));

      if(!parser.done) _parser_read_header(&parser);
      if(!parser.done) _parser_read_parameters(&parser);
      if(!parser.done) _parser_read_dt_entry(&parser);
      if(!parser.done) _parser_read_command(&parser);

      if(!parser.error)
      {
        command_list = g_list_append(command_list, parser.gmic_command);
      }
      else
      {
        _gmic_command_free(parser.gmic_command);
        parser.gmic_command = NULL;
      }
    }

    fclose(parser.pFile);
  }

  return command_list;
}

// load commands from all .gmic and .dtgmic files in {config dir}/GMIC/
GList *dt_load_gmic_commands_from_dir(const char *subdir)
{
  GList *temp_commands = NULL;
  const gchar *d_name;
  long fileSize = 0;
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  darktable.gmic_commands = NULL;
  darktable.gmic_custom_commands = NULL;

  char *dirname = g_build_filename(confdir, "GMIC", subdir, NULL);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
  {
    g_free(dirname);
    dirname = g_build_filename(datadir, "GMIC", subdir, NULL);
  }
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      char *filename = g_build_filename(dirname, d_name, NULL);
      const char *cc = filename + strlen(filename);
      for(; *cc != '.' && cc > filename; cc--)
        ;
      if(!g_ascii_strcasecmp(cc, ".dtgmic") || !g_ascii_strcasecmp(cc, ".dtgmic"))
      {
        GList *tmp_command = _load_gmic_commands_from_file(filename);
        if(tmp_command)
        {
          temp_commands = g_list_concat(temp_commands, tmp_command);
        }
      }
      else if(!g_ascii_strcasecmp(cc, ".gmic") || !g_ascii_strcasecmp(cc, ".gmic"))
      {
        FILE * pFile = fopen(filename, "rb" );
        if(pFile)
        {
          // obtain file size:
          fseek(pFile , 0 , SEEK_END);
          const long lSize = ftell(pFile);
          rewind(pFile);

          // allocate memory to contain the whole file:
          if(darktable.gmic_custom_commands == NULL)
            darktable.gmic_custom_commands = (char*)malloc(sizeof(char) * (lSize + 1));
          else
            darktable.gmic_custom_commands = (char*)realloc(darktable.gmic_custom_commands, sizeof(char) * (fileSize + lSize + 1));
          if(darktable.gmic_custom_commands)
          {
            // copy the file into the buffer:
            const size_t result = fread(darktable.gmic_custom_commands + fileSize, 1, lSize, pFile);
            if (result != (size_t)lSize)
              fprintf(stderr, "[dt_load_gmic_commands_from_dir] error reading custom commands file '%s'\n", filename);
            else
              darktable.gmic_custom_commands[lSize + fileSize] = 0;
            fileSize += lSize;
          }
          fclose(pFile);
        }
        else
          fprintf(stderr, "[dt_load_gmic_commands_from_dir] error opening custom commands file '%s'\n", filename);
      }
      g_free(filename);
    }
    g_dir_close(dir);
  }
  g_free(dirname);
  return temp_commands;
}

//------------------------------------------------------------------
// GMIC execution routines
//------------------------------------------------------------------

// in, out: a 4 channel image of width x height
// str: GMIC command
// scale_image: scale the image to [0..255] before sending to GMIC
//
// it sends the first 3 channels of in to GMIC and returns the same channels
// if GMIC returns a 1 channel image it sets the first 3 channel with that returned channel
void dt_gmic_run_3c(const float *const in, float *out, const int width, const int height, const char *str,
                    const gboolean scale_image)
{
  gmic_list<float> image_list;
  image_list.assign(1);
  gmic_list<char> image_names;
  const int ch = 4;

  {
    gmic_image<float> &img = image_list[0];
    img.assign(width, height, 1, ch - 1);
    float *ptr = img;
    const float scale = ((scale_image) ? 255.f : 1.f);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(img, ptr) schedule(static)
#endif
    for(unsigned int y = 0; y < img._height; ++y)
    {
      const float *g_in = in + (y * width * ch);

      for(unsigned int x = 0; x < img._width; ++x)
      {
        for(unsigned int c = 0; c < img._spectrum; ++c)
        {
          ptr[(c * img._width * img._height) + (y * img._width) + x] = g_in[(x * ch) + c] * scale;
        }
      }
    }
  }

  try
  {
    gmic(str, image_list, image_names, darktable.gmic_custom_commands);
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    printf("[dt_gmic_run_3c] error: %s\n", e.what());
    return;
  }

  {
    gmic_image<float> &img = image_list[0];
    const float *const ptr = image_list[0]._data;
    const float scale = ((scale_image) ? (1.f / 255.f) : 1.f);

    printf("[dt_gmic_run_3c] GMIC returned an image of width %i, height %i spectrum=%i of %i images\n",
        img._width, img._height, img._spectrum, image_list._width);

    const int _c = MIN(3, img._spectrum);
    const int _width = MIN(width, img._width);
    const int _height = MIN(height, img._height);

    if(img._spectrum == 1)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, img) schedule(static)
#endif
      for(int y = 0; y < _height; ++y)
      {
        float *g_out = out + (y * width * ch);

        for(int x = 0; x < _width; ++x)
        {
          g_out[(x * ch) + 0] = g_out[(x * ch) + 1] = g_out[(x * ch) + 2] = ptr[(y * img._width) + x] * scale;
        }
      }
    }
    else
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, img) schedule(static)
#endif
      for(int y = 0; y < _height; ++y)
      {
        float *g_out = out + (y * width * ch);

        for(int x = 0; x < _width; ++x)
        {
          for(int c = 0; c < _c; ++c)
          {
            g_out[(x * ch) + c] = ptr[(c * img._width * img._height) + (y * img._width) + x] * scale;
          }
        }
      }
    }
  }

  // Deallocate images.
  image_list.assign(0);
}

// in, out: a 4 channel image of width x height
// str: GMIC command
// scale_image: scale the image to [0..255] before sending to GMIC
//
// it sends the first channel of in to GMIC and returns the same channel
// if GMIC returns an image with more than one channel only the first is returned
void dt_gmic_run_1c(const float *const in, float *out, const int width, const int height, const char *str,
                    const gboolean scale_image)
{
  gmic_list<float> image_list;
  image_list.assign(1);
  gmic_list<char> image_names;
  const int ch = 4;

  {
    gmic_image<float> &img = image_list[0];
    img.assign(width, height, 1, 1);
    float *ptr = img;
    const float scale = ((scale_image) ? 255.f : 1.f);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(ptr, img) schedule(static)
#endif
    for(unsigned int y = 0; y < img._height; ++y)
    {
      const float *g_in = in + (y * width * ch);

      for(unsigned int x = 0; x < img._width; ++x)
      {
        ptr[(y * img._width) + x] = g_in[(x * ch)] * scale;
      }
    }
  }

  try
  {
    gmic(str, image_list, image_names, darktable.gmic_custom_commands);
  }
  catch(gmic_exception &e) // In case something went wrong.
  {
    printf("[dt_gmic_run_1c] error: %s\n", e.what());
    return;
  }

  {
    gmic_image<float> &img = image_list[0];
    const float *const ptr = image_list[0]._data;
    const float scale = ((scale_image) ? (1.f / 255.f) : 1.f);

    printf("[dt_gmic_run_1c] GMIC returned an image of width %i, height %i spectrum=%i of %i images\n",
        img._width, img._height, img._spectrum, image_list._width);

    const int _width = MIN(width, img._width);
    const int _height = MIN(height, img._height);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out, img) schedule(static)
#endif
    for(int y = 0; y < _height; ++y)
    {
      float *g_out = out + (y * width * ch);

      for(int x = 0; x < _width; ++x)
      {
        g_out[(x * ch)] = ptr[(y * img._width) + x] * scale;
      }
    }
  }

  // Deallocate images.
  image_list.assign(0);
}
}

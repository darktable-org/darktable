/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum token_types_t
{
  T_NUMBER, // everything will be treated as floats
  T_OPERATOR
} token_types_t;

typedef enum operators_t
{
  O_PLUS,
  O_INC,
  O_MINUS,
  O_DEC,
  O_MULTIPLY,
  O_DIVISION,
  O_MODULO,
  O_POWER,
  O_LEFTROUND,
  O_RIGHTROUND,
} operators_t;

typedef union token_data_t
{
  float number;
  operators_t operator;
} token_data_t;

typedef struct token_t
{
  token_types_t type;
  token_data_t data;
} token_t;

typedef struct parser_state_t
{
  char *p;
  float x;
  token_t *token;
} parser_state_t;

/** the scanner **/

static float read_number(parser_state_t *self)
{
  return g_ascii_strtod(self->p, &self->p);
}

static token_t *get_token(parser_state_t *self)
{
  if(!self->p) return NULL;

  token_t *token = (token_t *)malloc(sizeof(token_t));

  for(; *self->p; self->p++)
  {
    switch(*self->p)
    {
      case ' ':
      case '\t':
        continue;
      case '+':
        if(self->p[1] == '+')
        {
          self->p += 2;
          token->data.operator= O_INC;
        }
        else
        {
          self->p++;
          token->data.operator= O_PLUS;
        }
        token->type = T_OPERATOR;
        return token;
      case '-':
        if(self->p[1] == '-')
        {
          self->p += 2;
          token->data.operator= O_DEC;
        }
        else
        {
          self->p++;
          token->data.operator= O_MINUS;
        }
        token->type = T_OPERATOR;
        return token;
      case '*':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_MULTIPLY;
        return token;
      case '/':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_DIVISION;
        return token;
      case '%':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_MODULO;
        return token;
      case '^':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_POWER;
        return token;
      case '(':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_LEFTROUND;
        return token;
      case ')':
        self->p++;
        token->type = T_OPERATOR;
        token->data.operator= O_RIGHTROUND;
        return token;
      case 'x':
      case 'X':
        self->p++;
        token->type = T_NUMBER;
        token->data.number = self->x;
        return token;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '.':
      case ',':
      {
        token->data.number = read_number(self);
        token->type = T_NUMBER;
        return token;
      }
      default:
        // people complained about the messages when "TRUE" was fed to the calculator
        //         printf("error: %c\n", *self->p);
        break;
    }
  }

  free(token);
  return NULL;
}

/** the parser **/

static float parse_expression(parser_state_t *self);
static float parse_additive_expression(parser_state_t *self);
static float parse_multiplicative_expression(parser_state_t *self);
static float parse_power_expression(parser_state_t *self);
static float parse_unary_expression(parser_state_t *self);
static float parse_primary_expression(parser_state_t *self);

static float parse_expression(parser_state_t *self)
{
  return parse_additive_expression(self);
}

static float parse_additive_expression(parser_state_t *self)
{
  if(!self->token) return NAN;

  float left = parse_multiplicative_expression(self);

  while(self->token && self->token->type == T_OPERATOR)
  {
    const operators_t operator= self->token->data.operator;

    if(operator!= O_PLUS &&operator!= O_MINUS) return left;

    free(self->token);
    self->token = get_token(self);

    const float right = parse_multiplicative_expression(self);

    if(operator== O_PLUS)
      left += right;
    else if(operator== O_MINUS)
      left -= right;
  }

  return left;
}

static float parse_multiplicative_expression(parser_state_t *self)
{
  if(!self->token) return NAN;

  float left = parse_power_expression(self);

  while(self->token && self->token->type == T_OPERATOR)
  {
    const operators_t operator= self->token->data.operator;

    if(operator!= O_MULTIPLY &&operator!= O_DIVISION &&operator!= O_MODULO) return left;

    free(self->token);
    self->token = get_token(self);

    float right = parse_power_expression(self);

    if(operator== O_MULTIPLY)
      left *= right;
    else if(operator== O_DIVISION)
      left /= right;
    else if(operator== O_MODULO)
      left = fmodf(left, right);
  }

  return left;
}

static float parse_power_expression(parser_state_t *self)
{
  if(!self->token) return NAN;

  float left = parse_unary_expression(self);

  while(self->token && self->token->type == T_OPERATOR)
  {
    if(self->token->data.operator!= O_POWER) return left;

    free(self->token);
    self->token = get_token(self);

    const float right = parse_unary_expression(self);

    left = powf(left, right);
  }

  return left;
}

static float parse_unary_expression(parser_state_t *self)
{
  if(!self->token) return NAN;

  if(self->token->type == T_OPERATOR)
  {
    if(self->token->data.operator== O_MINUS)
    {
      free(self->token);
      self->token = get_token(self);

      return -1.0 * parse_unary_expression(self);
    }
    if(self->token->data.operator== O_PLUS)
    {
      free(self->token);
      self->token = get_token(self);

      return parse_unary_expression(self);
    }
  }

  return parse_primary_expression(self);
}

static float parse_primary_expression(parser_state_t *self)
{
  if(!self->token) return NAN;

  if(self->token->type == T_NUMBER)
  {
    const float result = self->token->data.number;
    free(self->token);
    self->token = get_token(self);
    return result;
  }
  if(self->token->type == T_OPERATOR && self->token->data.operator== O_LEFTROUND)
  {
    free(self->token);
    self->token = get_token(self);
    const float result = parse_expression(self);
    if(!self->token || self->token->type != T_OPERATOR || self->token->data.operator!= O_RIGHTROUND)
      return NAN;
    free(self->token);
    self->token = get_token(self);
    return result;
  }

  return NAN;
}

/** the public interface **/

float dt_calculator_solve(const float x, const char *formula)
{
  if(formula == NULL || *formula == '\0') return NAN;

  gchar *dotformula = g_strdup(formula);
  parser_state_t *self = (parser_state_t *)malloc(sizeof(parser_state_t));

  self->p = g_strdelimit(dotformula, ",", '.');
  self->x = x;

  self->token = get_token(self);

  float result = .0f;

  //   operators_t operator = -1;
  if(self->token && self->token->type == T_OPERATOR)
  {
    switch(self->token->data.operator)
    {
      case O_INC:
        result = x + 1.0;
        goto end;
      case O_DEC:
        result = x - 1.0;
        goto end;
      //       case O_PLUS:
      //       case O_MINUS:
      //       case O_MULTIPLY:
      //       case O_DIVISION:
      //       case O_MODULO:
      //       case O_POWER:
      //         operator = self->token->data.operator;
      //         free(self->token);
      //         self->token = get_token(self);
      //         break;
      default:
        break;
    }
  }

  result = parse_expression(self);

  //   switch(operator)
  //   {
  //     case O_PLUS: result = x + res; break;
  //     case O_MINUS: result = x - res; break;
  //     case O_MULTIPLY: result = x * res; break;
  //     case O_DIVISION: result = x / res; break;
  //     case O_MODULO: result = fmodf(x, res); break;
  //     case O_POWER: result = powf(x, res); break;
  //     default: break;
  //   }

  if(self->token) result = NAN;

end:
  free(self->token);
  free(self);
  g_free(dotformula);

  return result;
}

// int main()
// {
//   const char *input = "5/0";
//   float x = 3;
//
//   printf("%s\n", input);
//
//   float res = dt_calculator_solve(x, input);
//
//   printf("%f\n", res);
//
//   return 0;
// }

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on


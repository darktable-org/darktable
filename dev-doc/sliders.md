### Programmer's Guide: `dt_bauhaus_slider`

This guide provides a technical overview of the `dt_bauhaus_slider` widget in darktable for C/C++ developers. It details its usage, parameters, and the required order of operations for correct configuration.

---

### 1. Core Concepts

The `dt_bauhaus_slider` separates its internal data value from its displayed value. This is fundamental to its flexibility.

*   **Internal Value:** This is the raw `float` or `int` value as stored in the module's parameter struct (e.g., `dt_iop_exposure_params_t`). All range limits (`min`, `max`) and get/set operations (`dt_bauhaus_slider_get`, `dt_bauhaus_slider_set`) work with this internal value.
*   **Displayed Value:** This is the human-readable text shown in the GUI. It is derived from the internal value using the formula:
    `displayed_value = (internal_value * factor) + offset`
    This value is then formatted into a string using the `digits` and `format` properties.

### 2. Instantiation

*   **Introspection-based (Standard Method):** `dt_bauhaus_slider_from_params(self, "param_name")`
    This is the recommended method. It automatically configures the slider's range (`hard_min`, `hard_max`), default value, and data type by introspecting the module's parameter struct definition.

*   **Manual Creation:** `dt_bauhaus_slider_new_with_range(self, min, max, step, defval, digits)`
    This is used when a slider does not directly map to a parameter in the struct or requires a custom setup not supported by introspection.

### 3. Configuration Properties

#### 3.1. Range and Limits

The slider has two tiers of limits: "hard" and "soft".

*   **Hard Limits:** `dt_bauhaus_slider_set_hard_min()`, `dt_bauhaus_slider_set_hard_max()`
    These define the absolute, unbreakable boundaries of the slider's internal value. Values cannot be set beyond this range by any means.
*   **Soft Limits:** `dt_bauhaus_slider_set_soft_min()`, `dt_bauhaus_slider_set_soft_max()`, `dt_bauhaus_slider_set_soft_range()`
    These define the default visible range for the user. They provide a sensible working area, preventing users from accidentally selecting extreme values. The user can temporarily exceed these soft limits (up to the hard limits) by holding `Ctrl+Shift` while dragging the slider.

#### 3.2. Value Formatting and Display

These functions modify the `displayed_value` and have critical interdependencies.

*   `dt_bauhaus_slider_set_factor(widget, factor)`: Multiplies the internal value before display.
*   `dt_bauhaus_slider_set_offset(widget, offset)`: Adds to the value after applying the factor.
*   `dt_bauhaus_slider_set_format(widget, format_string)`: Appends a **literal string suffix** (e.g., `" EV"`, `" %"`) to the formatted number.
    *   **Peculiarity - Percentage (`%`):** This function contains a special heuristic. If the `format_string` contains a `%` character **and** `fabsf(hard_max) <= 10`, it assumes a percentage display is desired. It will automatically:
        1.  Set the `factor` to `100.0` (if it was the default `1.0`).
        2.  Perform a relative modification: `digits -= 2;`.
*   `dt_bauhaus_slider_set_digits(widget, digits)`: Sets the number of decimal places for the displayed number. This is the final step in formatting the numeric part of the display.

#### 3.3. Integration with Color Pickers

A slider can be associated with a color picker to allow its value to be set by sampling the image.

*   `dt_color_picker_new(self, flags, slider_widget)`: This function acts as a wrapper. It takes a newly created slider as an argument and returns a new `GtkWidget` that combines the slider and a picker button. The `GtkWidget` pointer in your GUI data struct should store the widget returned by `dt_color_picker_new`.

### 4. Internal Rounding and Configuration Order

#### 4.1. The Rounding Mechanism and Failure Mode

The slider's internal update function, `_slider_set_normalized()`, rounds the internal value to ensure it matches the displayed precision. The formula used is:
`rpos = roundf(base * rpos) / base;`
where `base = powf(10.0f, d->digits) * d->factor;`

A common incorrect configuration sequence can lead to `digits` becoming a negative number, which causes this formula to fail catastrophically.

**Example Failure Analysis:**
1.  A slider is created. Its default state is `digits = 0`, `factor = 1.0`.
2.  `dt_bauhaus_slider_set_format(slider, "%");` is called.
3.  The special "%" logic triggers. `factor` becomes `100.0`. `digits` is modified relative to its current value: `digits = 0 - 2 = -2`.
4.  The rounding `base` is now calculated as `pow(10, -2) * 100.0`, which equals `0.01 * 100.0 = 1.0`.
5.  The rounding formula becomes `rpos = roundf(1.0 * rpos) / 1.0;`, which is `rpos = roundf(rpos);`.

When this is applied to a slider with an internal range of `[0.0, 1.0]`, the internal value will be rounded to the nearest integer, meaning it can only ever be `0.0` or `1.0`. This is why the slider snaps to its ends (0% or 100%) and rejects any intermediate values.

#### 4.2. Recommended Configuration Order

To prevent this rounding failure and ensure predictable behavior, it is imperative to set the slider properties in an explicit, override-style order.

1.  **Set Transformation:** Configure the `factor` and `offset` first.
2.  **Set Format Suffix:** Call `dt_bauhaus_slider_set_format()`. This lets the convenience logic run, but its effects will be immediately corrected.
3.  **Set Final Precision:** Call `dt_bauhaus_slider_set_digits()` **last**. This sets the authoritative number of decimal places and overwrites any intermediate or incorrect values set by the `set_format` side effect.

### 5. Usage Recipes

#### Case 1: Plain Number (e.g., Gamma/Power)
A plain number with no units, displayed with 3 decimal places.

```c
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "contrast");
dt_bauhaus_slider_set_soft_range(slider, 0.5, 2.0);
dt_bauhaus_slider_set_digits(slider, 3);
// No call to set_format is needed for plain numbers.
```

#### Case 2: Percentage Display
An internal value of `-1.0` to `1.0` displayed as `-100.00 %` to `100.00 %`.

```c
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "saturation");

// 1. Set display factor.
dt_bauhaus_slider_set_factor(slider, 100.0f);
// 2. Set format suffix.
dt_bauhaus_slider_set_format(slider, " %");
// 3. Set final precision. This is the crucial last step.
dt_bauhaus_slider_set_digits(slider, 2);
```

#### Case 3: Degrees (`°`)
An angle stored in degrees, displayed with one decimal place.

```c
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "rotation");

dt_bauhaus_slider_set_format(slider, "°");
dt_bauhaus_slider_set_digits(slider, 1);
```

#### Case 3b: Radians to Degrees
An angle stored internally in radians but displayed in degrees.

```c
// Internal value is in radians (e.g., M_PI for 180°)
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "angle");

// RAD_2_DEG is defined in common/math.h (180/π ≈ 57.2957795)
dt_bauhaus_slider_set_factor(slider, RAD_2_DEG);
dt_bauhaus_slider_set_format(slider, "°");
dt_bauhaus_slider_set_digits(slider, 1);
```

#### Case 4: Simple Text Unit (e.g., EV, K)
An exposure value with a unit suffix and two decimal places.

```c
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "exposure");

// Note the leading space for UI padding.
dt_bauhaus_slider_set_format(slider, " EV");
dt_bauhaus_slider_set_digits(slider, 2);
```

#### Case 5: Slider with a Color Picker
An exposure slider linked to a color picker.

```c
dt_iop_agx_gui_data_t *gui_data = self->gui_data;

// 1. Create the slider first, but do not add it to a container yet.
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "range_white_relative_exposure");

// 2. Configure the slider using the recommended order.
dt_bauhaus_slider_set_soft_range(slider, 1.0f, 20.0f);
dt_bauhaus_slider_set_format(slider, " EV");
dt_bauhaus_slider_set_digits(slider, 2);

// 3. Create the color picker, passing the slider to it.
//    Store the widget returned by dt_color_picker_new().
gui_data->white_exposure_picker = dt_color_picker_new(
    self,
    DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,
    slider
);

// 4. Set the tooltip on the final, combined widget.
gtk_widget_set_tooltip_text(gui_data->white_exposure_picker,
                              _("relative exposure above mid-grey (white point)"));

// 5. Pack the combined widget into your UI.
//    (This is handled automatically if the slider was created with _from_params
//    and it is the first widget, but manual packing is shown here for clarity).
dt_gui_box_add(self->widget, gui_data->white_exposure_picker);
```

#### Case 6: Hue Slider with Rainbow Gradient
A hue slider displaying a color spectrum background using gradient stops.

```c
g->hue = dt_bauhaus_slider_from_params(self, "hue");

// Disable the moving indicator bar (not useful over a rainbow)
dt_bauhaus_slider_set_feedback(g->hue, 0);

// Internal 0.0-1.0 displayed as 0°-360°
dt_bauhaus_slider_set_factor(g->hue, 360.0f);
dt_bauhaus_slider_set_format(g->hue, "°");

// Add color stops for rainbow gradient (position, R, G, B)
// Position is normalized 0.0-1.0, colors are 0.0-1.0
dt_bauhaus_slider_set_stop(g->hue, 0.000f, 1.0f, 0.0f, 0.0f);  // Red
dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);  // Yellow
dt_bauhaus_slider_set_stop(g->hue, 0.333f, 0.0f, 1.0f, 0.0f);  // Green
dt_bauhaus_slider_set_stop(g->hue, 0.500f, 0.0f, 1.0f, 1.0f);  // Cyan
dt_bauhaus_slider_set_stop(g->hue, 0.666f, 0.0f, 0.0f, 1.0f);  // Blue
dt_bauhaus_slider_set_stop(g->hue, 0.833f, 1.0f, 0.0f, 1.0f);  // Magenta
dt_bauhaus_slider_set_stop(g->hue, 1.000f, 1.0f, 0.0f, 0.0f);  // Red (wrap)
```

#### Case 7: Saturation Slider with Dynamic Gradient

For a saturation slider paired with a hue slider, the gradient should update dynamically
to show grey at 0% and the current hue's saturated color at 100%.

```c
// In gui_init(): Set initial placeholder stops
g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
dt_bauhaus_slider_set_format(g->saturation, "%");
dt_bauhaus_slider_set_digits(g->saturation, 1);

// Initial stops (will be updated dynamically)
dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.2f, 0.2f, 0.2f);  // Gray
dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 1.0f, 0.0f, 0.0f);  // Red (placeholder)
```

Then create a helper function to update the gradient based on the current hue:

```c
// Helper to update saturation slider gradient based on hue
static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, 1.0, 0.5);  // Full saturation, mid lightness
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  if(w == g->hue)
  {
    update_saturation_slider_end_color(g->saturation, p->hue);
    gtk_widget_queue_draw(g->saturation);  // Force redraw
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  // Update gradient to match current hue
  update_saturation_slider_end_color(g->saturation, p->hue);

  gui_changed(self, NULL, NULL);
}
```

For a **fixed-hue** saturation slider (no accompanying hue control), use static stops:

```c
// Grey to red (fixed hue = 0)
dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.5f, 0.5f, 0.5f);  // Gray
dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 1.0f, 0.0f, 0.0f);  // Red

// Grey to cyan (fixed hue = 0.5)
dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.5f, 0.5f, 0.5f);  // Gray
dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 0.0f, 1.0f, 1.0f);  // Cyan
```

#### Case 8: Purity/Saturation Slider for a Primary Color (from agx.c)

A slider showing desaturated-to-saturated gradient for a specific hue (e.g., red at 0°):

```c
static GtkWidget *setup_purity_slider(dt_iop_module_t *self,
                                      const char *param_name,
                                      const float hue_deg,
                                      const gboolean attenuate)
{
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_feedback(slider, 0);  // Hide bar, gradient shows value
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);

  // Paint gradient using all stops
  dt_aligned_pixel_t hsv, rgb;
  for(int stop = 0; stop < DT_BAUHAUS_SLIDER_MAX_STOPS; stop++)
  {
    const float pos = (float)stop / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);

    hsv[0] = hue_deg / 360.0f;
    // attenuate: full color at 0%, grey at 100% (desaturation slider)
    // !attenuate: grey at 0%, full color at 100% (saturation slider)
    hsv[1] = attenuate ? 1.0f - pos : pos;
    hsv[2] = 1.0f;

    dt_HSV_2_RGB(hsv, rgb);
    dt_bauhaus_slider_set_stop(slider, pos, rgb[0], rgb[1], rgb[2]);
  }
  return slider;
}

// Usage:
g->red_inset = setup_purity_slider(self, "red_inset", 0.0f, TRUE);    // Red desaturation
g->green_outset = setup_purity_slider(self, "green_outset", 120.0f, FALSE); // Green saturation
```

#### Case 9: Hue Rotation Slider (from agx.c)

A slider showing hue range centered on a primary color (e.g., red ±60°):

```c
static GtkWidget *setup_hue_rotation_slider(dt_iop_module_t *self,
                                            const char *param_name,
                                            const float center_hue_deg,
                                            const gboolean reverse)
{
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_feedback(slider, 0);
  dt_bauhaus_slider_set_format(slider, "°");
  dt_bauhaus_slider_set_digits(slider, 1);
  dt_bauhaus_slider_set_factor(slider, RAD_2_DEG);  // Internal radians, display degrees

  const float hue_range_deg = 60.0f;
  dt_aligned_pixel_t hsv, rgb;

  for(int stop = 0; stop < DT_BAUHAUS_SLIDER_MAX_STOPS; stop++)
  {
    const float pos = (float)stop / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);

    // Offset from -60° to +60° across slider
    float hue_offset = -hue_range_deg + pos * (2.0f * hue_range_deg);
    if(reverse) hue_offset = -hue_offset;

    hsv[0] = fmodf(center_hue_deg + hue_offset + 360.0f, 360.0f) / 360.0f;
    hsv[1] = 0.7f;
    hsv[2] = 1.0f;

    dt_HSV_2_RGB(hsv, rgb);
    dt_bauhaus_slider_set_stop(slider, pos, rgb[0], rgb[1], rgb[2]);
  }
  return slider;
}

// Usage:
g->red_rotation = setup_hue_rotation_slider(self, "red_rotation", 0.0f, FALSE);
g->green_rotation = setup_hue_rotation_slider(self, "green_rotation", 120.0f, FALSE);
g->blue_rotation = setup_hue_rotation_slider(self, "blue_rotation", 240.0f, FALSE);
```

---

### 6. Additional Configuration Options

#### 6.1. Feedback (Moving Indicator Bar)

The slider normally shows a filled bar indicating the current position. This can be disabled:

```c
// 0 = no feedback bar, 1 = show feedback bar (default)
dt_bauhaus_slider_set_feedback(widget, 0);
```

Use this for sliders where the gradient background conveys the value (like hue sliders) or where the bar would be visually distracting.

#### 6.2. Gradient Color Stops

Add up to `DT_BAUHAUS_SLIDER_MAX_STOPS` (20) color stops to create a gradient background:

```c
// dt_bauhaus_slider_set_stop(widget, position, red, green, blue)
// - position: 0.0 to 1.0 (normalized slider range)
// - red/green/blue: 0.0 to 1.0

dt_bauhaus_slider_set_stop(slider, 0.0f, 0.0f, 0.0f, 0.0f);  // Black at start
dt_bauhaus_slider_set_stop(slider, 1.0f, 1.0f, 1.0f, 1.0f);  // White at end

// To remove all stops and return to default appearance:
dt_bauhaus_slider_clear_stops(slider);
```

**Soft vs Hard Range Alignment:** The gradient is painted across the *hard* range, but users
typically see the *soft* range. If your soft range differs from hard range, scale stop positions
accordingly (see agx.c `_paint_slider_gradient()` for a complete example):

```c
const float soft_min = dt_bauhaus_slider_get_soft_min(slider);
const float soft_max = dt_bauhaus_slider_get_soft_max(slider);
const float hard_min = dt_bauhaus_slider_get_hard_min(slider);
const float hard_max = dt_bauhaus_slider_get_hard_max(slider);

// For each stop position (0.0-1.0 in soft range):
const float value_in_soft = soft_min + position * (soft_max - soft_min);
const float normalized_pos = (value_in_soft - hard_min) / (hard_max - hard_min);
dt_bauhaus_slider_set_stop(slider, normalized_pos, r, g, b);
```

#### 6.3. Non-Linear Response Curves

For sliders where linear movement doesn't match perception (e.g., exposure), you can apply a transformation curve:

```c
// Use logarithmic curve (common for exposure-like values)
dt_bauhaus_slider_set_log_curve(widget);

// Or provide a custom curve function:
// The function maps normalized position (0-1) to/from value
static float my_curve(float value, dt_bauhaus_curve_t dir)
{
  if(dir == DT_BAUHAUS_SET)
    return /* transform value for display position */;
  else // DT_BAUHAUS_GET
    return /* transform display position back to value */;
}
dt_bauhaus_slider_set_curve(widget, my_curve);
```

#### 6.4. Step Size

Control the increment when using keyboard arrows or mouse scroll:

```c
// Default is (soft_max - soft_min) / 100
dt_bauhaus_slider_set_step(widget, 0.1f);
```

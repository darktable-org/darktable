# Introspection System

The **introspection system** in darktable is a powerful mechanism that allows the application to understand and manipulate the internal structure of C structs at runtime. It is primarily used for **Image Operation (IOP) modules** to define their parameters.

## Purpose

Introspection serves three main purposes:

1.  **Database Storage**: It allows module parameters (which are just C structs) to be serialized to and deserialized from the database as binary blobs, while handling versioning and upgrades (`legacy_params`).
2.  **GUI Generation**: The GUI logic (`DT_BAUHAUS_WIDGET` macros and functions) uses introspection data to automatically create sliders, comboboxes, and toggle buttons with the correct ranges, defaults, and labels.
3.  **Lua API**: It allows the Lua scripting interface to access and modify module parameters dynamically without writing explicit binding code for every field.

## Defining Introspection

Introspection is defined directly in the C code using the `DT_MODULE_INTROSPECTION` macro. This macro associates a struct type with a version number.

Example from `src/iop/exposure.c` (simplified):

```c
typedef struct dt_iop_exposure_params_t
{
  // $MIN: -3.0 $MAX: 3.0 $DEFAULT: 0.0 $DESCRIPTION: "EV shift"
  float exposure;

  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0
  float black;

  // $DEFAULT: 0
  int mode;
} dt_iop_exposure_params_t;

DT_MODULE_INTROSPECTION(1, dt_iop_exposure_params_t)
```

### Metadata Tags

The comments above each field are **parsed** during the build process to generate metadata. The supported tags are:

-   `$MIN`: The minimum allowed value.
-   `$MAX`: The maximum allowed value.
-   `$DEFAULT`: The default value.
-   `$DESCRIPTION`: A human-readable description (often used as a widget tooltip or label).
-   `$VALUES`: For enums, a list of valid values.

## Internal Structure

The introspection data is compiled into a tree of `dt_introspection_field_t` structures.

### `dt_introspection_t`
The top-level descriptor for a type.
-   `version`: The version number passed to `DT_MODULE_INTROSPECTION`.
-   `size`: The size of the struct in bytes.
-   `fields`: A list of `dt_introspection_field_t`.

### `dt_introspection_field_t`
Describes a single field in the struct. It is a union of various types (`Float`, `Int`, `Bool`, `Enum`, `Struct`, etc.), all sharing a common header.

#### `dt_introspection_type_header_t`
-   `type`: The type enum (`DT_INTROSPECTION_TYPE_FLOAT`, etc.).
-   `name`: The name of the field.
-   `offset`: The byte offset of the field within the parent struct.
-   `size`: The size of the field.

## Usage in GUI

The `dt_bauhaus_*` family of functions (e.g., `dt_bauhaus_slider_from_params`) uses introspection to bind a widget to a struct field.

When you call:
```c
g->exposure = dt_bauhaus_slider_from_params(self, "exposure");
```

1.  Darktable looks up the "exposure" field in the module's introspection data.
2.  It reads the `$MIN`, `$MAX`, and `$DEFAULT` values.
3.  It configures the slider range and default value.
4.  It binds the slider's value to the memory address `(char *)self->params + field->offset`.
5.  When the slider moves, the value at that address is updated automatically.

## Versioning

When you change the layout of a `params` struct, you **must** increment the introspection version number.

```c
DT_MODULE_INTROSPECTION(2, dt_iop_exposure_params_t)
```

You must then implement the `legacy_params` function in your module to migrate data from the old version (1) to the new version (2). This ensures that edits made with older versions of darktable are preserved.

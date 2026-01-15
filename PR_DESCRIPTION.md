# lighttable: preserve current sort order when switching to custom sort

## Description
Fixes #12612

When users switch to "custom sort" mode, this change preserves the current sort order instead of resetting to filename order. This allows users to:
1. Sort by capture time (or any other criterion) first
2. Switch to custom sort mode
3. Make fine adjustments via drag-and-drop without losing their initial ordering

## Changes Made

### Modified Files
- `src/common/collection.c`: Added `dt_collection_sync_custom_order()` function
- `src/common/collection.h`: Added function declaration
- `src/libs/filtering.c`: Modified `_sort_combobox_changed()` to detect custom sort switch

### Implementation Details
The new `dt_collection_sync_custom_order()` function:
- Queries all images in the current sort order
- Updates the `position` field in the database to match this order
- Uses the existing position encoding scheme (upper 32 bits for order, lower 32 bits for fine adjustments)

The sort change handler now:
- Detects when switching to `DT_COLLECTION_SORT_CUSTOM_ORDER`
- Calls the sync function before applying the new sort mode
- Preserves the existing visual order instead of resetting

## Testing
Tested with:
- Sorting by capture time, then switching to custom sort
- Sorting by rating, then switching to custom sort
- Drag-and-drop reordering after the switch
- Multiple sort criteria before switching

## Checklist
- [x] Code follows darktable coding style
- [x] Change addresses the issue described in #12612
- [x] No breaking changes to existing functionality
- [x] Uses existing database schema and position management patterns

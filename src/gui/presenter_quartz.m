/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/*
 * the macOS platform of the layered presenter backend. macOS color-manages
 * every tagged layer, so a window is always native. the platform watches
 * the window's screen for its EDR headroom: the current value once the
 * image layer has shown EDR content (macOS reports 1.0 before), the
 * screen's potential headroom as the ceiling, a new generation when the
 * window moves to another screen. the transport puts
 * two views above GTK's content view, which take no input: the image layer
 * is a CAMetalLayer tagged extended linear BT.2020 with EDR on, carrying
 * the image in half floats, 1.0 at SDR white, and macOS does the one
 * conversion to the screen. the foreground layer above it is an 8-bit sRGB
 * CAMetalLayer, sRGB being what GTK tags its own surface with (3.24.37 on,
 * which the build requires). see dev-doc/display_presentation.md.
 */

// the system headers AppKit pulls in need the BSD types that darktable's
// _XOPEN_SOURCE hides
#define _DARWIN_C_SOURCE

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "gui/presenter_layers.h"
#include "common/darktable.h"

#include <gdk/gdkquartz.h>
#if __has_include(<gdk/quartz/gdkquartz-cocoa-access.h>)
#include <gdk/quartz/gdkquartz-cocoa-access.h>
#endif

// a layer takes no input: GTK's view below keeps getting the events
@interface DtPresenterLayerView : NSView
@end

@implementation DtPresenterLayerView
- (NSView *)hitTest:(NSPoint)point
{
  return nil;
}
@end

typedef struct _layer_t
{
  DtPresenterLayerView *view;
  CAMetalLayer *layer;
  // what the layer is copied from, two so that one can be written while
  // the GPU still reads the other, each with the command buffer that last
  // read it
  id<MTLBuffer> buffers[2];
  id<MTLCommandBuffer> readers[2];
  int reserved;
  // the drawable reserve() took for commit(), released by the next one
  id<CAMetalDrawable> drawable;
  size_t stride;
  int width, height;
} _layer_t;

typedef struct _widget_t
{
  _layer_t image, foreground;
  dt_presenter_layers_t layers;
} _widget_t;

typedef struct _platform_t
{
  dt_presenter_platform_t platform;
  dt_presenter_window_t *window;
  GtkWidget *toplevel;
  NSWindow *nswindow;
  NSView *parent;
  id observers[3];
  uint32_t generation;
  // the image layer has shown EDR content: macOS reports the screen's
  // current headroom from then on, 1.0 before
  gboolean engaged;
} _platform_t;

typedef struct _transport_t
{
  dt_presenter_transport_t transport;
  _platform_t *p;
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  GdkFrameClock *clock;
  gulong paint_handler;
  CFRunLoopObserverRef observer;
  // the headroom target the image is encoded for
  float headroom;
  GList *widgets;
} _transport_t;

/* headroom */

static void _report(_platform_t *p)
{
  NSScreen *screen = p->nswindow.screen ? p->nswindow.screen : NSScreen.mainScreen;
  if(!screen) return;
  const float reference = screen.maximumReferenceExtendedDynamicRangeColorComponentValue;
  const float potential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  const float current = screen.maximumExtendedDynamicRangeColorComponentValue;
  // a reference preset fixes the headroom; otherwise it follows the
  // brightness, and the ramp after EDR content appears ends at or below
  // the potential headroom
  const dt_presenter_headroom_t report =
    { .value = reference > 0.0f ? reference : (p->engaged ? current : NAN),
      .ceiling = reference > 0.0f ? reference : potential,
      .generation = p->generation };
  dt_print(DT_DEBUG_CONTROL | DT_DEBUG_VERBOSE,
           "[presenter quartz] %s window: current %.4f, potential %.4f, reference %.4f,"
           " engaged %d, generation %u",
           p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           current, potential, reference, p->engaged, p->generation);
  dt_presenter_window_report_headroom(p->window, &report);
}

static void _new_generation(_platform_t *p)
{
  p->generation++;
  _report(p);
}

// a display woken up has EDR off until the image layer presents again. it
// is the same screen, so the target from before the sleep stays, and a
// value counts again after the next present, through the usual settling.
// a target that did not change draws nothing by itself: ask for that present
static void _wake(_platform_t *p)
{
  p->engaged = FALSE;
  _report(p);
  dt_presenter_window_redraw(p->window);
}

/* layers */

static void _layer_init(_transport_t *t,
                        _layer_t *layer,
                        const gboolean image)
{
  layer->view = [[DtPresenterLayerView alloc] initWithFrame:NSZeroRect];
  layer->layer = [[CAMetalLayer alloc] init];
  layer->layer.device = t->device;
  layer->layer.opaque = NO;
  // the layers are written by blits, never rendered to
  layer->layer.framebufferOnly = NO;
  // the drawable goes on screen with the Core Animation transaction that
  // also carries GTK's drawing of the frame (_present())
  layer->layer.presentsWithTransaction = YES;
  CGColorSpaceRef space =
    CGColorSpaceCreateWithName(image ? kCGColorSpaceExtendedLinearITUR_2020 : kCGColorSpaceSRGB);
  layer->layer.colorspace = space;
  CGColorSpaceRelease(space);
  if(image)
  {
    layer->layer.pixelFormat = MTLPixelFormatRGBA16Float;
    layer->layer.wantsExtendedDynamicRangeContent = YES;
  }
  else
    layer->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer->view.layer = layer->layer;
  layer->view.wantsLayer = YES;
  layer->view.hidden = YES;
  // above GTK's drawing and everything added before
  [t->p->parent addSubview:layer->view positioned:NSWindowAbove relativeTo:nil];
}

static void _layer_wait(_layer_t *layer)
{
  for(int k = 0; k < 2; k++)
    if(layer->readers[k]) [layer->readers[k] waitUntilCompleted];
}

static void _layer_free(_layer_t *layer)
{
  _layer_wait(layer);
  [layer->view removeFromSuperview];
  [layer->view release];
  [layer->layer release];
  for(int k = 0; k < 2; k++)
  {
    [layer->buffers[k] release];
    [layer->readers[k] release];
  }
  [layer->drawable release];
  memset(layer, 0, sizeof(_layer_t));
}

// a buffer of the layer the GPU is done with, for width x height pixels of
// bpp bytes, and the drawable commit() copies it to; NULL while the GPU
// reads both buffers or the layer has no drawable free
static void *_layer_reserve(_transport_t *t,
                            _layer_t *layer,
                            const int width,
                            const int height,
                            const int scale,
                            const int bpp)
{
  // one not committed goes back to the layer
  [layer->drawable release];
  layer->drawable = nil;

  const size_t stride = (size_t)bpp * width;
  if(stride != layer->stride || height != layer->height)
  {
    _layer_wait(layer);
    for(int k = 0; k < 2; k++)
    {
      [layer->buffers[k] release];
      [layer->readers[k] release];
      layer->buffers[k] = nil;
      layer->readers[k] = nil;
    }
    layer->stride = stride;
    layer->width = width;
    layer->height = height;
  }

  for(int k = 0; k < 2; k++)
  {
    id<MTLCommandBuffer> reader = layer->readers[k];
    if(reader && reader.status < MTLCommandBufferStatusCompleted) continue;
    if(!layer->buffers[k])
      layer->buffers[k] = [t->device newBufferWithLength:stride * height
                                                 options:MTLResourceStorageModeShared];
    if(!layer->buffers[k]) return NULL;
    layer->reserved = k;
    layer->layer.drawableSize = CGSizeMake(width, height);
    layer->layer.contentsScale = scale;
    // blocks for up to a second while every drawable is on screen or queued
    layer->drawable = [[layer->layer nextDrawable] retain];
    return layer->drawable ? layer->buffers[k].contents : NULL;
  }
  return NULL;
}

// copy the reserved buffer to the reserved drawable and present it with the
// current Core Animation transaction
static void _layer_commit(_transport_t *t,
                          _layer_t *layer)
{
  id<CAMetalDrawable> drawable = layer->drawable;
  layer->drawable = nil;

  id<MTLCommandBuffer> commands = [t->queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromBuffer:layer->buffers[layer->reserved]
          sourceOffset:0
     sourceBytesPerRow:layer->stride
   sourceBytesPerImage:layer->stride * layer->height
            sourceSize:MTLSizeMake(layer->width, layer->height, 1)
             toTexture:drawable.texture
      destinationSlice:0
      destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [commands commit];
  [layer->readers[layer->reserved] release];
  layer->readers[layer->reserved] = [commands retain];
  // presentsWithTransaction: present once the copy is scheduled, inside
  // the transaction
  [commands waitUntilScheduled];
  [drawable present];
  [drawable release];
}

/* transport interface */

static gboolean _reserve(dt_presenter_transport_t *self,
                         GtkWidget *widget,
                         void **widget_data,
                         const dt_presenter_layers_t *layers,
                         dt_presenter_reservation_t *reservation)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;
  if(!w)
  {
    w = g_malloc0(sizeof(_widget_t));
    _layer_init(t, &w->image, TRUE);
    _layer_init(t, &w->foreground, FALSE);
    t->widgets = g_list_prepend(t->widgets, w);
    *widget_data = w;
  }

  // everything commit() needs is taken here, so that it cannot fail. the
  // foreground goes with every present: without its buffer or drawable the
  // present is refused and tried again next frame. without the image's,
  // the layered backend refuses it only if the image changed
  uint8_t *foreground = NULL;
  uint16_t *image = NULL;
  @autoreleasepool
  {
    foreground = _layer_reserve(t, &w->foreground, layers->width, layers->height,
                                layers->scale, 4);
    if(foreground)
      image = _layer_reserve(t, &w->image, layers->width, layers->height, layers->scale, 8);
  }
  if(!foreground) return FALSE;
  w->layers = *layers;

  // EDR: 1.0 is SDR white and values above it use the screen's headroom,
  // which macOS clips to. the target is what the pipes render up to
  const float headroom = t->headroom;
  *reservation = (dt_presenter_reservation_t){
    .foreground = foreground,
    .foreground_stride = w->foreground.stride,
    .image = image,
    .image_stride = w->image.stride,
    .encoding = { .clip = { 0.0f, headroom },
                  .matrix = { { 1.0f, 0.0f, 0.0f },
                              { 0.0f, 1.0f, 0.0f },
                              { 0.0f, 0.0f, 1.0f } } } };
  return TRUE;
}

static void _place(_transport_t *t,
                   _layer_t *layer,
                   const dt_presenter_layers_t *layers)
{
  // layers are in the toplevel's logical coordinates, from the top left
  const NSRect bounds = t->p->parent.bounds;
  const CGFloat width = (CGFloat)layers->width / layers->scale;
  const CGFloat height = (CGFloat)layers->height / layers->scale;
  const CGFloat y = t->p->parent.isFlipped ? layers->y : bounds.size.height - layers->y - height;
  layer->view.frame = NSMakeRect(layers->x, y, width, height);
  layer->view.hidden = NO;
}

static void _commit(dt_presenter_transport_t *self,
                    void **widget_data,
                    const gboolean image)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;

  // drawables are autoreleased: hand them back with this frame, not with
  // whatever pool GDK drains later, or the layer runs out of them
  @autoreleasepool
  {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _place(t, &w->image, &w->layers);
    _place(t, &w->foreground, &w->layers);
    // an image drawable taken for a present that keeps the image shown is
    // released by the next reserve()
    if(image) _layer_commit(t, &w->image);
    _layer_commit(t, &w->foreground);
    // the image layer is on screen, whether this present replaced it or
    // kept it, so EDR is engaged and macOS reports the screen's headroom
    if(!t->p->engaged)
    {
      t->p->engaged = TRUE;
      _report(t->p);
    }
    [CATransaction commit];
  }
}

static void _clear(dt_presenter_transport_t *self,
                   void **widget_data)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;
  // without the image layer on screen macOS winds EDR down: a value counts
  // again once a layer presents
  t->p->engaged = FALSE;
  // hidden with the transaction of the frame that cleared them
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  w->image.view.hidden = YES;
  w->foreground.view.hidden = YES;
  [CATransaction commit];
}

static void _release(dt_presenter_transport_t *self,
                     void **widget_data)
{
  _transport_t *t = (_transport_t *)self;
  _widget_t *w = *widget_data;
  if(!w) return;
  _layer_free(&w->foreground);
  _layer_free(&w->image);
  t->widgets = g_list_remove(t->widgets, w);
  g_free(w);
  *widget_data = NULL;
}

static void _free(dt_presenter_transport_t *self)
{
  _transport_t *t = (_transport_t *)self;
  if(t->paint_handler) g_signal_handler_disconnect(t->clock, t->paint_handler);
  if(t->observer)
  {
    CFRunLoopRemoveObserver(CFRunLoopGetMain(), t->observer, kCFRunLoopCommonModes);
    CFRelease(t->observer);
  }
  for(GList *l = t->widgets; l; l = g_list_next(l))
  {
    _widget_t *w = l->data;
    _layer_free(&w->foreground);
    _layer_free(&w->image);
    g_free(w);
  }
  g_list_free(t->widgets);
  [t->queue release];
  [t->device release];
  // without the image layer macOS winds EDR down: what it reports says
  // nothing until a new transport shows EDR content again. no report from
  // here, which runs while the window may be going away
  t->p->engaged = FALSE;
  g_free(t);
}

static void _set_output(_transport_t *t);

static void _target(dt_presenter_transport_t *self,
                    const float headroom)
{
  _transport_t *t = (_transport_t *)self;
  // only the clip range changes, so reservations encode for the new target
  // at once and it is published now. the image shown was composed for the
  // old range: the new one composes it again (presenter_layers.c,
  // _image_changed())
  t->headroom = headroom;
  _set_output(t);
}

// GTK3's Quartz backend runs the draw handlers of a frame from the frame
// clock's paint phase, and the frame's drawing reaches the screen with the
// Core Animation transaction committed at the end of that run loop pass.
// presenting at the end of paint puts the layers in the same transaction;
// the run loop observer, which runs just before Core Animation commits,
// catches a draw made outside paint
static void _paint(GdkFrameClock *clock,
                   _transport_t *t)
{
  dt_presenter_window_present(t->p->window);
}

static void _set_output(_transport_t *t)
{
  // GTK tags its own surface sRGB (3.24.37 on). the image headroom is the
  // one reservations encode for. Core Animation offers no rendering intent:
  // both spaces are matrix based, and a matrix conversion is relative
  // colorimetric
  const dt_display_output_t output =
    { .image = { .type = DT_COLORSPACE_LIN_REC2020, .intent = DT_INTENT_RELATIVE_COLORIMETRIC,
                 .headroom = t->headroom },
      .device = { .type = DT_COLORSPACE_SRGB, .intent = DT_INTENT_RELATIVE_COLORIMETRIC,
                  .headroom = 1.0f },
      .format = DT_FRAME_FLOAT };
  dt_presenter_window_set_output(t->p->window, &output);
}

static dt_presenter_transport_t *_transport_new(dt_presenter_platform_t *self)
{
  _platform_t *p = (_platform_t *)self;
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  dt_print(DT_DEBUG_CONTROL, "[presenter quartz] %s window: %s",
           p->window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           queue ? "EDR layers" : "no Metal device, using cairo");
  if(!queue)
  {
    [device release];
    return NULL;
  }

  _transport_t *t = g_malloc0(sizeof(_transport_t));
  t->p = p;
  t->device = device;
  t->queue = queue;
  t->headroom = dt_presenter_window_target(p->window);
  t->transport.reserve = _reserve;
  t->transport.commit = _commit;
  t->transport.clear = _clear;
  t->transport.release = _release;
  t->transport.free = _free;
  t->transport.target = _target;
  // GTK3 on Quartz draws popovers into the toplevel's view, below the
  // layers: the layered backend replays them on the foreground
  t->transport.popovers_above = FALSE;
  t->clock = gdk_window_get_frame_clock(gtk_widget_get_window(p->toplevel));
  t->paint_handler = g_signal_connect_after(t->clock, "paint", G_CALLBACK(_paint), t);
  t->observer = CFRunLoopObserverCreateWithHandler
    (NULL, kCFRunLoopBeforeWaiting, TRUE, 1999999,
     ^(CFRunLoopObserverRef observer, CFRunLoopActivity activity) {
       dt_presenter_window_present(t->p->window);
     });
  CFRunLoopAddObserver(CFRunLoopGetMain(), t->observer, kCFRunLoopCommonModes);
  _set_output(t);
  return &t->transport;
}

/* platform interface */

static gboolean _native(dt_presenter_platform_t *self)
{
  return TRUE;
}

static void _platform_free(dt_presenter_platform_t *self)
{
  _platform_t *p = (_platform_t *)self;
  for(int k = 0; k < G_N_ELEMENTS(p->observers); k++)
    if(p->observers[k])
    {
      NSNotificationCenter *center = k == 2 ? NSWorkspace.sharedWorkspace.notificationCenter
                                            : NSNotificationCenter.defaultCenter;
      [center removeObserver:p->observers[k]];
      [p->observers[k] release];
    }
  [p->parent release];
  [p->nswindow release];
  g_free(p);
}

dt_presenter_platform_t *dt_presenter_quartz_new(GtkWidget *toplevel,
                                                 dt_presenter_window_t *window)
{
  GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
  if(!GDK_IS_QUARTZ_WINDOW(gdk_window)) return NULL;
  NSWindow *nswindow = gdk_quartz_window_get_nswindow(gdk_window);
  NSView *parent = gdk_quartz_window_get_nsview(gdk_window);
  if(!nswindow || !parent) return NULL;

  _platform_t *p = g_malloc0(sizeof(_platform_t));
  p->window = window;
  p->toplevel = toplevel;
  p->nswindow = [nswindow retain];
  p->parent = [parent retain];

  NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
  // the screen's headroom moves with the brightness and while an EDR ramp
  // runs, about every 8 ms; the core settles its target from the reports
  p->observers[0] =
    [[center addObserverForName:NSApplicationDidChangeScreenParametersNotification
                         object:nil
                          queue:nil
                     usingBlock:^(NSNotification *note) { _report(p); }] retain];
  // what was reported on another screen says nothing about this one
  p->observers[1] =
    [[center addObserverForName:NSWindowDidChangeScreenNotification
                         object:nswindow
                          queue:nil
                     usingBlock:^(NSNotification *note) { _new_generation(p); }] retain];
  p->observers[2] =
    [[NSWorkspace.sharedWorkspace.notificationCenter
       addObserverForName:NSWorkspaceScreensDidWakeNotification
                   object:nil
                    queue:nil
               usingBlock:^(NSNotification *note) { _wake(p); }] retain];

  p->platform.native = _native;
  p->platform.transport = _transport_new;
  p->platform.free = _platform_free;

  // the first report: no value until the image layer has engaged EDR
  _report(p);
  return &p->platform;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

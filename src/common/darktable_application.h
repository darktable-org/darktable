//
// Created by mikesolar on 25-10-21.
//

#ifndef DARKTABLE_APPLICATION_H
#define DARKTABLE_APPLICATION_H
#include "darktable_application.h"
#include "gio/gio.h"
int handle_command(GApplication   *application,
                    gchar        ***arguments,
                    gint           *exit_status
);
typedef GApplication DarkTableApplication;
typedef GApplicationClass DarkTableApplicationClass;

GType darktable_application_get_type (void);
G_DEFINE_TYPE(DarkTableApplication, darktable_application, G_TYPE_APPLICATION)

void darktable_application_finalize (GObject *object);

static void darktable_application_init (DarkTableApplication *app)
{
}

static void darktable_application_class_init (DarkTableApplicationClass *class)
{
  G_OBJECT_CLASS (class)->finalize = darktable_application_finalize;
  G_APPLICATION_CLASS (class)->local_command_line = handle_command;
}

GApplication *darktable_application_new (const gchar       *application_id,
                      GApplicationFlags  flags);

#endif //DARKTABLE_APPLICATION_H

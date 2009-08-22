/*
  ScriptExec - binary bundled into Platypus-created applications
  Copyright (C) 2003 Sveinbjorn Thordarson <sveinbt@hi.is>
  
  Gimp.app specific modifications:
  Copyright (C) 2006 Aaron Voisine <aaron@voisine.org>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
  I would like to thank Gianni Ceccarelli for his contribution of modified code
  which enabled piping from a privileged task into a Text Window
*/

#import "ScriptExecController.h"
#include <sys/wait.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

@implementation ScriptExecController

- (id)init
{
    if (self = [super init]) {
        files = [[NSMutableArray alloc] initWithCapacity:1024];
    }
    return self;
}

-(void)dealloc
{
    if (files != NULL)  [files release];
    [super dealloc];
}

- (void)applicationDidFinishLaunching: (NSNotification *)aNotification
{
    int i = 0;
    char *args[[files count] + 2];
    NSEnumerator *enumerator = [files objectEnumerator];
    id filename;

    args[i++] = strdup([[[NSBundle mainBundle] pathForResource:@"script"
                                               ofType:nil] UTF8String]);
    while (filename = [enumerator nextObject]) {
        args[i++] = strdup([filename UTF8String]);
    }
    args[i] = NULL;

    execv(args[0], args);
}

#pragma mark -

// check if task is running
- (void)checkTaskStatus
{
    if ([files count] > 0) {
        NSMutableArray *args = [[NSMutableArray alloc]
                                   initWithCapacity:[files count] + 1];
        [args addObject:[[NSBundle mainBundle] pathForResource:@"openDoc"
                                               ofType:nil]];
        [args addObjectsFromArray:files];
        [files removeAllObjects];
        
        NSTask *openDocTask = [[NSTask alloc] init];
        [openDocTask setLaunchPath:@"/bin/sh"];
        [openDocTask setArguments:args];
        [openDocTask launch];
        [openDocTask waitUntilExit];

        [openDocTask release];
        [args release];
    }
}

#pragma mark -

// respond to AEOpenDoc -- so MUCH more convenient than working with
// Apple Event Descriptors
- (BOOL)application:(NSApplication *)theApplication
           openFile:(NSString *)filename
{
    [files addObject:filename];
    return TRUE;
}

- (NSApplicationTerminateReply)applicationShouldTerminate: 
    (NSApplication *)sender
{
    return(YES);
}

#pragma mark -

// Respond to Cancel by exiting application
- (IBAction)cancel:(id)sender
{
    [[NSApplication sharedApplication] terminate:self];
}

#pragma mark -

- (void)fatalAlert:(NSString *)message subText:(NSString *)subtext
{
    NSAlert *alert = [[NSAlert alloc] init];
    [alert addButtonWithTitle:@"OK"];
    [alert setMessageText:message];
    [alert setInformativeText:subtext];
    [alert setAlertStyle:NSCriticalAlertStyle];
	
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        [alert release];
        [[NSApplication sharedApplication] terminate:self];
    } 
}

@end

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

#import <Cocoa/Cocoa.h>

@interface ScriptExecController : NSObject
{
    IBOutlet id progressBar;
    IBOutlet id progressWindow;

    NSTask *initTask;
    NSTask *scriptTask;
    NSMutableArray *files;

    bool scriptStarted;
    int count;
}
- (void)checkTaskStatus;
- (IBAction)cancel:(id)sender;
- (void)fatalAlert:(NSString *)message subText:(NSString *)subtext;
@end

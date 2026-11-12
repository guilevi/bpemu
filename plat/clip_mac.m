/* clip_mac.m --- clipboard files on macOS: NSPasteboard file URLs.  */

#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <string.h>

#include "clip.h"

int
clip_files (char ***out)
{
  *out = NULL;
  @autoreleasepool
  {
#ifdef CLIP_TEST_PASTEBOARD	/* tests use their own, not the user's */
    NSPasteboard *pb = [NSPasteboard pasteboardWithName:@CLIP_TEST_PASTEBOARD];
#else
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
#endif
    NSArray *urls = [pb
      readObjectsForClasses:@[[NSURL class]]
		    options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if (!urls.count)
      return 0;
    char **v = calloc (urls.count, sizeof *v);
    int n = 0;
    for (NSURL *u in urls)
      {
	const char *p = u.fileSystemRepresentation;
	if (p)
	  v[n++] = strdup (p);
      }
    *out = v;
    return n;
  }
}

void
clip_free (char **paths, int n)
{
  for (int i = 0; i < n; i++)
    free (paths[i]);
  free (paths);
}

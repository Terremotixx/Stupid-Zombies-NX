/* editbox.c -- libnx software-keyboard bridge for the engine's EditBox/TextBox.
 *
 * Ported from the acpc_nx port. The engine drives text entry through JNI
 * ShowEditBox / IsOpenEditBox / GetEditBoxText / CloseEditBox: it renders its own
 * field and polls for the text, so all we have to do is pop the Switch software
 * keyboard when asked and hand the result back.
 *
 * swkbdShow() is BLOCKING (it runs the system applet), so editbox_show() does not
 * return until the user confirms or cancels. Unity's Android keyboard is
 * asynchronous, however: the Java open/configuration call returns before Java
 * reports the typed string and close event. Delivering those callbacks directly
 * inside editbox_show() is reentrant and lets the rest of Unity's open path
 * overwrite the new text with its initial value. We therefore queue the result
 * and deliver it from editbox_pump() after nativeRender has returned.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>

#include "editbox.h"

extern int debugPrintf(char *text, ...);
extern unsigned g_kbd_trace;   /* jni_fake.c: log the next N object calls */
extern void kbd_push_result(const char *text, int cancelled);  /* jni_fake.c */

#define EDITBOX_TEXT_CAP 1024

static char g_editbox_text[EDITBOX_TEXT_CAP];
static int  g_editbox_open;
static int  g_editbox_cancelled;
static int  g_editbox_pending;

void editbox_show(const char *initial, int maxlen) {
  if (g_editbox_open || g_editbox_pending) {
    debugPrintf("[kbd] ignored duplicate open while result is pending\n");
    return;
  }
  g_editbox_open = 1;
  g_editbox_cancelled = 1;                  /* assume cancel until confirmed */

  if (!initial) initial = "";
  snprintf(g_editbox_text, sizeof g_editbox_text, "%s", initial);
  if (maxlen <= 0 || maxlen >= EDITBOX_TEXT_CAP) maxlen = EDITBOX_TEXT_CAP - 1;

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    char result[EDITBOX_TEXT_CAP];
    snprintf(result, sizeof result, "%s", g_editbox_text);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetInitialText(&kbd, g_editbox_text);
    swkbdConfigSetStringLenMax(&kbd, (u32)maxlen);
    rc = swkbdShow(&kbd, result, sizeof result);   /* blocks: system applet */
    if (R_SUCCEEDED(rc)) {
      snprintf(g_editbox_text, sizeof g_editbox_text, "%s", result);
      g_editbox_cancelled = 0;
    }
    swkbdClose(&kbd);
  }

  g_editbox_open = 0;
  debugPrintf("[kbd] swkbd closed: cancelled=%d text=\"%s\"\n",
              g_editbox_cancelled, g_editbox_text);
  g_editbox_pending = 1;
  debugPrintf("[kbd] result queued until nativeRender returns\n");
}

int editbox_is_open(void) { return g_editbox_open || g_editbox_pending; }

void editbox_pump(void) {
  if (!g_editbox_pending) return;
  g_editbox_pending = 0;
  g_kbd_trace = 60;   /* trace the calls Unity makes after accepting the result */
  debugPrintf("[kbd] delivering queued result after nativeRender\n");
  /* Unity PUSHES soft-input results: its Java keyboard calls
   * nativeSetInputString + nativeSoftInputClosed. We are that Java side. */
  kbd_push_result(g_editbox_text, g_editbox_cancelled);
}

const char *editbox_text(void) {
  /* On cancel, report the text unchanged from what the engine seeded us with,
   * which is what a cancelled edit should look like. */
  return g_editbox_text;
}

void editbox_close(void) {
  /* The applet owns the UI while it is up, so there is nothing to dismiss here;
   * record the cancel so a later editbox_text() reflects it. */
  if (g_editbox_open) g_editbox_cancelled = 1;
  (void)g_editbox_cancelled;
}

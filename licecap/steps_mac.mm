/*
  steps_mac.mm -- platform half of the text guide for LICEcap.

  Owns a listen-only CGEventTap (mouse downs + keydowns) running on the main
  run loop (same thread as SWELL's WM_TIMER, so no locking is required), and
  turns raw events into guide text using CGWindowList (app + window title,
  needs Screen Recording -- already required by recording itself) and the
  Accessibility API (element role/title, optional -- degrades gracefully).

  Keystroke *contents* are never read; only burst counts are recorded.
*/

#include "steps.h"

#ifdef LICECAP_STEPS

#import <Cocoa/Cocoa.h>
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>

#include "../WDL/swell/swell.h" // timeGetTime -- same clock as the capture timer
#include "steps_int.h"

// AXIsProcessTrusted is deprecated since 10.9 (AXIsProcessTrustedWithOptions
// supersedes it) but remains the right fallback for pre-10.9 targets.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static CFMachPortRef s_tap = NULL;
static CFRunLoopSourceRef s_src = NULL;
static bool s_collect = false;

static StepEvent s_q[STEPS_MAX_EVENTS];
static int s_qhead = 0, s_qlen = 0;

// ------------------------------------------------------------------- queue

static void q_push(const StepEvent &ev)
{
  if (s_qlen >= STEPS_MAX_EVENTS) // drop oldest on overflow (pathological bursts)
  {
    s_qhead = (s_qhead + 1) % STEPS_MAX_EVENTS;
    --s_qlen;
  }
  s_q[(s_qhead + s_qlen) % STEPS_MAX_EVENTS] = ev;
  ++s_qlen;
}

void stepsplat_setcollecting(bool en)
{
  s_collect = en;
  if (!en) { s_qhead = 0; s_qlen = 0; }
}

void stepsplat_clearpending() { s_qhead = 0; s_qlen = 0; }

int stepsplat_drain(StepEvent *out, int maxOut)
{
  int n = s_qlen;
  if (n > maxOut) n = maxOut;
  for (int i = 0; i < n; ++i)
    out[i] = s_q[(s_qhead + i) % STEPS_MAX_EVENTS];
  s_qhead = 0;
  s_qlen = 0; // any remainder beyond maxOut is discarded
  return n;
}

// --------------------------------------------------------------------- tap

static CGEventRef steps_tap_cb(CGEventTapProxy proxy, CGEventType type,
                               CGEventRef event, void *refcon)
{
  (void)proxy; (void)refcon;

  if (type == kCGEventTapDisabledByTimeout ||
      type == kCGEventTapDisabledByUserInput)
  {
    if (s_tap) CGEventTapEnable(s_tap, true);
    return event;
  }
  if (!s_collect) return event;

  StepEvent ev;
  memset(&ev, 0, sizeof(ev));
  switch (type)
  {
    case kCGEventLeftMouseDown:  ev.kind = 1; break;
    case kCGEventRightMouseDown: ev.kind = 2; break;
    case kCGEventOtherMouseDown: ev.kind = 3; break;
    case kCGEventKeyDown:
    {
      const int64_t kc =
        CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
      if (kc >= 54 && kc <= 63) return event; // modifier-only presses (shift/ctrl/cmd/opt/caps/fn)
      ev.kind = 4;
      break;
    }
    default: return event;
  }

  ev.t = timeGetTime();
  if (ev.kind != 4)
  {
    const CGPoint p = CGEventGetLocation(event);
    ev.x = (int)p.x;
    ev.y = (int)p.y;
  }
  ev.mods = (unsigned int)CGEventGetFlags(event);
  q_push(ev);
  return event;
}

bool stepsplat_init()
{
  if (s_tap) return true;

  const CGEventMask mask =
      CGEventMaskBit(kCGEventLeftMouseDown)
    | CGEventMaskBit(kCGEventRightMouseDown)
    | CGEventMaskBit(kCGEventOtherMouseDown)
    | CGEventMaskBit(kCGEventKeyDown);

  s_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                           kCGEventTapOptionListenOnly, mask,
                           steps_tap_cb, NULL);
  if (!s_tap) return false;

  s_src = CFMachPortCreateRunLoopSource(NULL, s_tap, 0);
  if (!s_src)
  {
    CFRelease(s_tap);
    s_tap = NULL;
    return false;
  }
  // registered from a UI callback -- main run loop, same thread as WM_TIMER
  CFRunLoopAddSource(CFRunLoopGetCurrent(), s_src, kCFRunLoopCommonModes);
  return true;
}

void stepsplat_shutdown()
{
  if (s_src)
  {
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), s_src, kCFRunLoopCommonModes);
    CFRelease(s_src);
    s_src = NULL;
  }
  if (s_tap)
  {
    CFMachPortInvalidate(s_tap);
    CFRelease(s_tap);
    s_tap = NULL;
  }
  s_qhead = 0;
  s_qlen = 0;
}

bool stepsplat_tapok() { return s_tap != NULL; }

// -------------------------------------------------------------- permissions

bool stepsplat_checkpermissions(bool prompt)
{
  bool ok = true;

  if (@available(macOS 10.15, *))
  {
    if (!CGPreflightListenEventAccess())
    {
      if (prompt) CGRequestListenEventAccess();
      ok = false;
    }
  }

  bool axok;
  if (@available(macOS 10.9, *))
  {
    if (prompt)
    {
      NSDictionary *opts = [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:YES]
                                                       forKey:(id)kAXTrustedCheckOptionPrompt];
      axok = AXIsProcessTrustedWithOptions((CFDictionaryRef)opts);
    }
    else axok = AXIsProcessTrusted();
  }
  else axok = AXIsProcessTrusted();

  if (!axok) ok = false;
  return ok;
}

// ---------------------------------------------------------------- describing

static bool ax_trusted()
{
  if (@available(macOS 10.9, *)) return AXIsProcessTrusted() != false;
  return AXIsProcessTrusted() != false;
}

static void copy_cfstr(CFStringRef s, char *out, int outsz)
{
  if (!out || outsz < 1) return;
  out[0] = 0;
  if (s) CFStringGetCString(s, out, outsz, kCFStringEncodingUTF8);
}

// "AXTextField" -> "text field", "AXCheckBox" -> "check box", ...
static void humanize_role(const char *role, char *out, int outsz)
{
  out[0] = 0;
  if (!role || outsz < 1) return;
  if (!strncmp(role, "AX", 2)) role += 2;
  int oi = 0;
  bool first = true;
  for (; *role && oi < outsz - 1; ++role, first = false)
  {
    char c = *role;
    if (c >= 'A' && c <= 'Z')
    {
      if (!first && oi < outsz - 2) out[oi++] = ' ';
      c = (char)(c - 'A' + 'a');
    }
    out[oi++] = c;
  }
  out[oi] = 0;
}

static void app_name(int pid, char *out, int outsz)
{
  out[0] = 0;
  if (pid <= 0) return;
  NSRunningApplication *a =
    [NSRunningApplication runningApplicationWithProcessIdentifier:(pid_t)pid];
  if (a)
  {
    const char *n = [[a localizedName] UTF8String];
    if (n) snprintf(out, outsz, "%s", n);
  }
}

static bool bounds_from_dict(CFDictionaryRef b, CGRect *out)
{
  if (!b) return false;
  CFNumberRef xn = (CFNumberRef)CFDictionaryGetValue(b, CFSTR("X"));
  CFNumberRef yn = (CFNumberRef)CFDictionaryGetValue(b, CFSTR("Y"));
  CFNumberRef wn = (CFNumberRef)CFDictionaryGetValue(b, CFSTR("Width"));
  CFNumberRef hn = (CFNumberRef)CFDictionaryGetValue(b, CFSTR("Height"));
  if (!xn || !yn || !wn || !hn) return false;
  double x = 0, y = 0, w = 0, h = 0;
  CFNumberGetValue(xn, kCFNumberDoubleType, &x);
  CFNumberGetValue(yn, kCFNumberDoubleType, &y);
  CFNumberGetValue(wn, kCFNumberDoubleType, &w);
  CFNumberGetValue(hn, kCFNumberDoubleType, &h);
  out->origin.x = x; out->origin.y = y;
  out->size.width = w; out->size.height = h;
  return true;
}

// Front-most on-screen window containing (x,y). Returns 1 when that window
// belongs to LICEcap itself (step should be skipped), 0 when found
// (name/app/pid filled, any may be empty), -1 when nothing matched.
static int window_at_point(int x, int y,
                           char *winNm, int winNmSz,
                           char *appNm, int appNmSz,
                           int *pidOut)
{
  winNm[0] = 0;
  appNm[0] = 0;
  *pidOut = 0;

  CFArrayRef list = CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID);
  if (!list) return -1;

  const CFIndex n = CFArrayGetCount(list);
  int ret = -1;
  for (CFIndex i = 0; i < n; ++i) // list is ordered front to back
  {
    CFDictionaryRef w = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
    CGRect r;
    if (!bounds_from_dict((CFDictionaryRef)CFDictionaryGetValue(w, kCGWindowBounds), &r))
      continue;
    if (x < r.origin.x || y < r.origin.y ||
        x >= r.origin.x + r.size.width || y >= r.origin.y + r.size.height)
      continue;

    int pid = 0;
    CFNumberRef pn = (CFNumberRef)CFDictionaryGetValue(w, kCGWindowOwnerPID);
    if (pn) CFNumberGetValue(pn, kCFNumberIntType, &pid);
    if (pid == (int)getpid()) { ret = 1; break; } // our own UI

    copy_cfstr((CFStringRef)CFDictionaryGetValue(w, kCGWindowName),
               winNm, winNmSz);
    copy_cfstr((CFStringRef)CFDictionaryGetValue(w, kCGWindowOwnerName),
               appNm, appNmSz);
    *pidOut = pid;
    ret = 0;
    break;
  }
  CFRelease(list);
  return ret;
}

// name of pid's front-most window (for keystroke context)
static void front_window_name(int pid, char *out, int outsz)
{
  out[0] = 0;
  if (pid <= 0) return;
  CFArrayRef list = CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID);
  if (!list) return;
  const CFIndex n = CFArrayGetCount(list);
  for (CFIndex i = 0; i < n; ++i)
  {
    CFDictionaryRef w = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
    int wpid = 0;
    CFNumberRef pn = (CFNumberRef)CFDictionaryGetValue(w, kCGWindowOwnerPID);
    if (pn) CFNumberGetValue(pn, kCFNumberIntType, &wpid);
    if (wpid != pid) continue;
    copy_cfstr((CFStringRef)CFDictionaryGetValue(w, kCGWindowName), out, outsz);
    break;
  }
  CFRelease(list);
}

// AX element under the point: "“OK” button in window “Save” (Finder)".
// Returns false when AX is unavailable or the lookup failed.
static bool ax_describe_click(int x, int y, int pid, char *core, int coreSz)
{
  core[0] = 0;
  if (!ax_trusted() || pid <= 0) return false;

  AXUIElementRef appEl = AXUIElementCreateApplication(pid);
  if (!appEl) return false;
  AXUIElementSetMessagingTimeout(appEl, 0.3f);

  AXUIElementRef el = NULL;
  const bool got =
    AXUIElementCopyElementAtPosition(appEl, (float)x, (float)y, &el) ==
      kAXErrorSuccess && el != NULL;
  if (got)
  {
    char role[64] = "", title[256] = "", roleHuman[64] = "";
    char winTitle[256] = "", appNm[128] = "";
    CFTypeRef v;

    if (AXUIElementCopyAttributeValue(el, kAXRoleAttribute, &v) ==
        kAXErrorSuccess && v)
    {
      copy_cfstr((CFStringRef)v, role, sizeof(role));
      CFRelease(v);
    }
    if (AXUIElementCopyAttributeValue(el, kAXTitleAttribute, &v) ==
        kAXErrorSuccess && v)
    {
      copy_cfstr((CFStringRef)v, title, sizeof(title));
      CFRelease(v);
    }
    if (!title[0] &&
        AXUIElementCopyAttributeValue(el, kAXDescriptionAttribute, &v) ==
          kAXErrorSuccess && v)
    {
      copy_cfstr((CFStringRef)v, title, sizeof(title));
      CFRelease(v);
    }
    AXUIElementRef winEl = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXWindowAttribute, &v) ==
        kAXErrorSuccess && v)
    {
      winEl = (AXUIElementRef)v;
      CFTypeRef wv;
      if (AXUIElementCopyAttributeValue(winEl, kAXTitleAttribute, &wv) ==
          kAXErrorSuccess && wv)
      {
        copy_cfstr((CFStringRef)wv, winTitle, sizeof(winTitle));
        CFRelease(wv);
      }
      CFRelease(winEl);
    }
    CFRelease(el);

    humanize_role(role, roleHuman, sizeof(roleHuman));
    app_name(pid, appNm, sizeof(appNm));

    char part[400];
    if (title[0] && roleHuman[0])
      snprintf(part, sizeof(part), "\xe2\x80\x9c%s\xe2\x80\x9d %s", title, roleHuman);
    else if (title[0])
      snprintf(part, sizeof(part), "\xe2\x80\x9c%s\xe2\x80\x9d", title);
    else if (roleHuman[0])
      snprintf(part, sizeof(part), "%s", roleHuman);
    else part[0] = 0;

    if (part[0] && winTitle[0] && appNm[0])
      snprintf(core, coreSz, "%s in window \xe2\x80\x9c%s\xe2\x80\x9d (%s)",
               part, winTitle, appNm);
    else if (part[0] && winTitle[0])
      snprintf(core, coreSz, "%s in window \xe2\x80\x9c%s\xe2\x80\x9d", part, winTitle);
    else if (part[0] && appNm[0])
      snprintf(core, coreSz, "%s (%s)", part, appNm);
    else if (part[0])
      snprintf(core, coreSz, "%s", part);
  }
  CFRelease(appEl);
  return got;
}

int stepsplat_describe(const StepEvent *ev, char *coreOut, int coreSz,
                       char *ctxOut, int ctxSz)
{
  if (ev->kind == 4)
  {
    if (ctxOut && ctxSz > 0)
    {
      ctxOut[0] = 0;
      NSRunningApplication *front = [[NSWorkspace sharedWorkspace] frontmostApplication];
      const int pid = front ? (int)[front processIdentifier] : 0;
      if (pid == (int)getpid()) return 1; // typing into LICEcap itself

      char appNm[128] = "";
      app_name(pid, appNm, sizeof(appNm));

      if (ax_trusted() && pid > 0)
      {
        AXUIElementRef appEl = AXUIElementCreateApplication(pid);
        if (appEl)
        {
          AXUIElementSetMessagingTimeout(appEl, 0.3f);
          CFTypeRef foc = NULL;
          if (AXUIElementCopyAttributeValue(appEl, kAXFocusedUIElementAttribute, &foc) ==
                kAXErrorSuccess && foc)
          {
            char role[64] = "", title[256] = "", roleHuman[64] = "";
            CFTypeRef v;
            if (AXUIElementCopyAttributeValue((AXUIElementRef)foc, kAXRoleAttribute, &v) ==
                  kAXErrorSuccess && v)
            {
              copy_cfstr((CFStringRef)v, role, sizeof(role));
              CFRelease(v);
            }
            if (AXUIElementCopyAttributeValue((AXUIElementRef)foc, kAXTitleAttribute, &v) ==
                  kAXErrorSuccess && v)
            {
              copy_cfstr((CFStringRef)v, title, sizeof(title));
              CFRelease(v);
            }
            humanize_role(role, roleHuman, sizeof(roleHuman));

            if (roleHuman[0] && title[0])
              snprintf(ctxOut, ctxSz, "in \xe2\x80\x9c%s\xe2\x80\x9d %s (%s)",
                       title, roleHuman, appNm[0] ? appNm : "an application");
            else if (roleHuman[0])
              snprintf(ctxOut, ctxSz, "in %s (%s)",
                       roleHuman, appNm[0] ? appNm : "an application");
            CFRelease(foc);
          }
          CFRelease(appEl);
        }
      }
      if (!ctxOut[0])
      {
        char win[256] = "";
        front_window_name(pid, win, sizeof(win));
        if (win[0] && appNm[0])
          snprintf(ctxOut, ctxSz, "in window \xe2\x80\x9c%s\xe2\x80\x9d (%s)", win, appNm);
        else if (appNm[0])
          snprintf(ctxOut, ctxSz, "in %s", appNm);
        else
          snprintf(ctxOut, ctxSz, "in an application");
      }
    }
    return 0;
  }

  // mouse step
  if (coreOut && coreSz > 0)
  {
    coreOut[0] = 0;
    char win[256] = "", appNm[128] = "";
    int pid = 0;
    const int wr = window_at_point(ev->x, ev->y, win, sizeof(win),
                                   appNm, sizeof(appNm), &pid);
    if (wr == 1) return 1; // click landed on LICEcap's own UI

    char axcore[512];
    if (wr == 0 && ax_describe_click(ev->x, ev->y, pid, axcore, sizeof(axcore)))
      snprintf(coreOut, coreSz, "%s", axcore);
    else if (wr == 0 && win[0] && appNm[0])
      snprintf(coreOut, coreSz, "in window \xe2\x80\x9c%s\xe2\x80\x9d (%s)", win, appNm);
    else if (wr == 0 && appNm[0])
      snprintf(coreOut, coreSz, "in %s", appNm);
    else
      snprintf(coreOut, coreSz, "at (%d, %d)", ev->x, ev->y);
  }
  return 0;
}

#pragma clang diagnostic pop

void Steps_RequestPermissions(bool prompt)
{
  stepsplat_checkpermissions(prompt);
}

#endif // LICECAP_STEPS

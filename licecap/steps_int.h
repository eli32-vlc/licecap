/*
  steps_int.h -- internal interface between steps.cpp (core: session state,
  coalescing, thumbnails, HTML export) and steps_mac.mm (platform: CGEventTap,
  Accessibility / CGWindowList descriptions, permission prompts).

  Only compiled when LICECAP_STEPS is defined (macOS Xcode target).
*/

#ifndef _LICECAP_STEPS_INT_H_
#define _LICECAP_STEPS_INT_H_

#define STEPS_MAX_EVENTS 256

struct StepEvent
{
  unsigned int t;      // timeGetTime() at event
  int x, y;            // CG global display coords (origin top-left of main display, y down)
  int kind;            // 1 = left down, 2 = right down, 3 = other down, 4 = key down
  unsigned int mods;   // CGEventFlags (raw)
};

// -- implemented in steps_mac.mm ------------------------------------------

// create the listen-only event tap (idempotent); false if creation failed
// (e.g. Input Monitoring not granted yet -- safe to retry each session)
bool stepsplat_init();

// gate the tap callback; when false, incoming events are dropped and any
// queued events are discarded (called on pause/resume/stop -- main thread)
void stepsplat_setcollecting(bool en);

// discard queued events without processing them
void stepsplat_clearpending();

// move up to maxOut queued events into out; returns count moved
int stepsplat_drain(StepEvent *out, int maxOut);

// Turn an event into guide text.
//   kind 1-3 (mouse): writes the phrase *after* the click verb into coreOut,
//      e.g. "\u201cOK\u201d button in window \u201cSave\u201d (Finder)" or
//      "in window \u201cSave\u201d (Finder)" or "at (123, 456)".
//   kind 4 (key): writes the context phrase into ctxOut,
//      e.g. "in text field \u201cSearch\u201d (Safari)".
// Returns 1 if the step should be skipped (event belongs to LICEcap itself),
// 0 on success (may still return partial/fallback text).
int stepsplat_describe(const StepEvent *ev, char *coreOut, int coreSz,
                       char *ctxOut, int ctxSz);

// permission preflight; if prompt is true, triggers the system prompts
// (Input Monitoring on 10.15+, Accessibility) when not yet granted.
// returns true when both are granted (AX may legitimately be denied -- guide
// then falls back to window-level descriptions).
bool stepsplat_checkpermissions(bool prompt);

// true if a working event tap currently exists
bool stepsplat_tapok();

// release the tap (next session re-creates it; lets newly granted
// Input Monitoring permission take effect)
void stepsplat_shutdown();

#endif // _LICECAP_STEPS_INT_H_

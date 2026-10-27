/*
  steps.h -- text guide (step recorder) support for LICEcap, macOS.

  While recording, user interactions (mouse clicks, keystrokes) are observed
  via a listen-only CGEventTap; each step is described using the Accessibility
  API (falling back to CGWindowList when AX is unavailable), gets a downscaled
  thumbnail of the captured frame, and at Stop the guide is written as a
  single self-contained HTML file next to the recording
  (e.g. movie.gif -> movie_steps.html).

  On platforms/builds where LICECAP_STEPS is not defined, all entry points
  are inline no-ops so licecap_ui.cpp compiles unchanged (e.g. Windows).
*/

#ifndef _LICECAP_STEPS_H_
#define _LICECAP_STEPS_H_

class LICE_IBitmap;

#ifdef LICECAP_STEPS

// permissions (macOS): prompts for Input Monitoring + Accessibility if needed
void Steps_RequestPermissions(bool prompt);

// session lifecycle -- driven by licecap_ui.cpp recording state changes.
// enable = whether the guide option was on when recording started.
void Steps_OnRecordStart(bool enable);
void Steps_OnRecordPause();    // stops collecting (pending events dropped)
void Steps_OnRecordResume();   // collection resumes; timeline excludes pause
void Steps_OnRecordStop();     // collection off; keeps steps for export

// called once per captured frame while actively recording, right after the
// cursor overlay is drawn. bm = full-res frame in logical (output) orientation,
// capLeft/capMinY = the exact (xpos, ypos) arguments passed to GetScreenData()
// this frame (they define where the frame sits on screen), now = timeGetTime().
void Steps_OnFrame(LICE_IBitmap *bm, int capLeft, int capMinY, unsigned int now);

int Steps_Count();

// writes <recordingFn minus ext>_steps.html if a session recorded any steps.
// returns true and hands back the output path on success.
bool Steps_Export(const char *recordingFn, char *outPath, int outPathSz);

#else // !LICECAP_STEPS -- no-op stubs

static inline void Steps_RequestPermissions(bool) {}
static inline void Steps_OnRecordStart(bool) {}
static inline void Steps_OnRecordPause() {}
static inline void Steps_OnRecordResume() {}
static inline void Steps_OnRecordStop() {}
static inline void Steps_OnFrame(LICE_IBitmap *, int, int, unsigned int) {}
static inline int Steps_Count() { return 0; }
static inline bool Steps_Export(const char *, char *, int) { return false; }

#endif // LICECAP_STEPS

#endif // _LICECAP_STEPS_H_

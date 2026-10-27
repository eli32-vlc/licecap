/*
  steps.cpp -- text guide core for LICEcap: session state, event coalescing,
  per-step thumbnails (downscaled frame + click marker, encoded with a minimal
  built-in PNG writer on top of WDL's zlib), and self-contained HTML export.

  Platform services (event tap, AX/CGWindowList descriptions) live in
  steps_mac.mm behind steps_int.h. Compiled only when LICECAP_STEPS is defined.
*/

#include "steps.h"

#ifdef LICECAP_STEPS

#include "steps_int.h"
#include "licecap_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../WDL/wdltypes.h"
#include "../WDL/swell/swell.h" // timeGetTime -- same clock as the capture timer
#include "../WDL/lice/lice.h"
#include "../WDL/wdl_base64.h"
#include "../WDL/zlib/zlib.h"

#include <ApplicationServices/ApplicationServices.h>

#define STEPS_MAX          200   // hard cap; oldest steps dropped beyond this
#define STEPS_THUMB_W      480   // thumbnail width in px (native if smaller)
#define STEPS_KEY_MERGE_MS 1500  // keydowns closer than this coalesce into one step
#define STEPS_DBL_MS       400   // near-simultaneous clicks coalesce (double-click)
#define STEPS_DBL_PX       8

struct StepRec
{
  unsigned int t;         // event time (timeGetTime)
  bool isKey;
  int keyCount;           // keystrokes merged into this step
  int mx, my;             // click marker in thumbnail coords, -1 = none
  char desc[512];         // mouse step text (verb already included)
  char ctx[256];          // key step context, e.g. "in Safari"
  char thumb[320];        // temp PNG path, "" = no thumbnail
};

static StepRec s_steps[STEPS_MAX];
static int s_count;
static bool s_armed;        // session active (guide enabled + recording)
static bool s_paused;
static bool s_exported;
static int  s_truncated;
static unsigned int s_t0;         // timeGetTime of first captured frame
static unsigned int s_pausedAcc;  // total paused ms (after t0)
static unsigned int s_pauseStart;
static unsigned int s_durMs;      // final duration, set at stop
static char s_tmpdir[512];
static int  s_seq;        // thumbnail filename counter (unique within session)

static unsigned int s_lastKeyT;
static unsigned int s_lastMouseT;
static int s_lastMouseX, s_lastMouseY, s_lastMouseKind;
static bool s_haveLastMouse;

// ---------------------------------------------------------------- helpers

static unsigned int steps_elapsed(unsigned int t)
{
  if (!s_t0) return 0;
  int v = (int)(t - s_t0) - (int)s_pausedAcc;
  return v > 0 ? (unsigned int)v : 0;
}

static void steps_format_ms(unsigned int ms, char *out, int outsz)
{
  unsigned int s = ms / 1000;
  snprintf(out, outsz, "%u:%02u.%1u", s / 60, s % 60, (ms % 1000) / 100);
}

static void html_escape(FILE *f, const char *p)
{
  for (; *p; ++p)
  {
    switch (*p)
    {
      case '&':  fputs("&amp;", f); break;
      case '<':  fputs("&lt;", f); break;
      case '>':  fputs("&gt;", f); break;
      case '"':  fputs("&quot;", f); break;
      case '\'': fputs("&#39;", f); break;
      default:   fputc(*p, f);
    }
  }
}

static void steps_cleanup_thumbs()
{
  for (int i = 0; i < s_count; ++i)
    if (s_steps[i].thumb[0]) unlink(s_steps[i].thumb);
  if (s_tmpdir[0]) rmdir(s_tmpdir);
  s_tmpdir[0] = 0;
}

// ------------------------------------------------- minimal PNG (RGB, 8 bit)

static void png_put32(unsigned char *p, unsigned int v)
{
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static bool png_write_chunk(FILE *f, const char *type, const void *data, int len)
{
  unsigned char hdr[4];
  png_put32(hdr, (unsigned int)len);
  if (fwrite(hdr, 1, 4, f) != 4) return false;
  if (fwrite(type, 1, 4, f) != 4) return false;
  if (len && fwrite(data, 1, (size_t)len, f) != (size_t)len) return false;

  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, (const Bytef *)type, 4);
  if (len) crc = crc32(crc, (const Bytef *)data, (uInt)len);
  png_put32(hdr, (unsigned int)crc);
  return fwrite(hdr, 1, 4, f) == 4;
}

// bm must be a plain (unflipped) bitmap; row 0 = top, as PNG expects.
static bool steps_write_png(const char *fn, LICE_IBitmap *bm)
{
  const int w = bm->getWidth(), h = bm->getHeight();
  if (w < 1 || h < 1) return false;

  const size_t stride = 1 + (size_t)w * 3;
  unsigned char *raw = (unsigned char *)malloc(stride * (size_t)h);
  if (!raw) return false;

  const int span = bm->getRowSpan();
  const LICE_pixel *bits = bm->getBits();
  for (int y = 0; y < h; ++y)
  {
    unsigned char *row = raw + (size_t)y * stride;
    row[0] = 0; // filter: none
    const LICE_pixel *src = bits + (size_t)y * (size_t)span;
    unsigned char *d = row + 1;
    for (int x = 0; x < w; ++x, d += 3)
    {
      const LICE_pixel px = src[x];
      d[0] = (unsigned char)(px & 0xff);        // R (LICE_RGBA packs R low)
      d[1] = (unsigned char)((px >> 8) & 0xff); // G
      d[2] = (unsigned char)((px >> 16) & 0xff);// B
    }
  }

  uLongf clen = compressBound(stride * (size_t)h);
  unsigned char *cbuf = (unsigned char *)malloc(clen);
  if (!cbuf) { free(raw); return false; }
  if (compress2(cbuf, &clen, raw, stride * (size_t)h, 6) != Z_OK)
  {
    free(raw); free(cbuf);
    return false;
  }
  free(raw);

  FILE *f = fopen(fn, "wb");
  if (!f) { free(cbuf); return false; }

  static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  bool ok = fwrite(sig, 1, 8, f) == 8;

  unsigned char ihdr[13];
  png_put32(ihdr, (unsigned int)w);
  png_put32(ihdr + 4, (unsigned int)h);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 2;   // color type: truecolor RGB
  ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  if (ok) ok = png_write_chunk(f, "IHDR", ihdr, 13);
  if (ok) ok = png_write_chunk(f, "IDAT", cbuf, (int)clen);
  if (ok) ok = png_write_chunk(f, "IEND", NULL, 0);
  free(cbuf);
  fclose(f);
  return ok;
}

// ------------------------------------------------------------- thumbnails

// Creates <s_tmpdir>/s<idx>.png: frame scaled to STEPS_THUMB_W, with a red
// click marker when (mkx,mky) >= 0 (full-res logical coords). Path is stored
// into out path. Returns false if the file could not be written.
static bool steps_make_thumb(LICE_IBitmap *frame, int mkx, int mky,
                             int *omx, int *omy,
                             char *pathOut, int pathSz)
{
  const int bw = frame->getWidth(), bh = frame->getHeight();
  if (bw < 1 || bh < 1 || !s_tmpdir[0]) return false;

  int tw = bw < STEPS_THUMB_W ? bw : STEPS_THUMB_W;
  int th = (int)((long long)bh * tw / bw);
  if (th < 1) th = 1;

  LICE_IBitmap *thumb = new LICE_MemBitmap(tw, th);
  LICE_Clear(thumb, 0xff000000);
  LICE_ScaledBlit(thumb, frame, 0, 0, tw, th,
                  0, 0, (float)bw, (float)bh, 1.0f, LICE_BLIT_MODE_COPY);

  *omx = *omy = -1;
  if (mkx >= 0 && mky >= 0 && mkx < bw && mky < bh)
  {
    const int x = (int)((long long)mkx * tw / bw);
    const int y = (int)((long long)mky * th / bh);
    // white ring + red core, matching the recorder's click-highlight style
    LICE_Circle(thumb, (float)x + 1.0f, (float)y + 1.0f, 10.0f,
                LICE_RGBA(255, 255, 255, 255), 1.0f, LICE_BLIT_MODE_COPY, true);
    LICE_Circle(thumb, (float)x + 1.0f, (float)y + 1.0f, 8.0f,
                LICE_RGBA(226, 40, 40, 255), 1.0f, LICE_BLIT_MODE_COPY, true);
    *omx = x;
    *omy = y;
  }

  snprintf(pathOut, pathSz, "%s/s%04d.png", s_tmpdir, ++s_seq);
  const bool ok = steps_write_png(pathOut, thumb);
  delete thumb;
  if (!ok) pathOut[0] = 0;
  return ok;
}

// ---------------------------------------------------------------- session

void Steps_OnRecordStart(bool enable)
{
  // drop any previous (unexported) session
  steps_cleanup_thumbs();
  s_count = 0;
  s_truncated = 0;
  s_exported = false;
  s_t0 = s_pausedAcc = s_pauseStart = s_durMs = 0;
  s_paused = false;
  s_armed = false;
  s_haveLastMouse = false;
  s_lastKeyT = 0;
  stepsplat_setcollecting(false);
  stepsplat_clearpending();

  if (!enable) return;

  // recreate the tap if missing/stale so freshly granted Input Monitoring
  // permission takes effect for this session
  if (!stepsplat_checkpermissions(false) || !stepsplat_tapok())
    stepsplat_shutdown();
  stepsplat_init();

  const char *tmp = getenv("TMPDIR");
  snprintf(s_tmpdir, sizeof(s_tmpdir), "%s/lcesteps_%d",
           tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
  mkdir(s_tmpdir, 0700);
  s_seq = 0;

  s_armed = true;
  stepsplat_setcollecting(true);
}

void Steps_OnRecordPause()
{
  if (!s_armed || s_paused) return;
  s_paused = true;
  s_pauseStart = timeGetTime();
  stepsplat_setcollecting(false); // drops pending events too
}

void Steps_OnRecordResume()
{
  if (!s_armed || !s_paused) return;
  s_paused = false;
  // time before the first frame (preroll) is already excluded by s_t0
  if (s_t0) s_pausedAcc += timeGetTime() - s_pauseStart;
  stepsplat_clearpending();
  stepsplat_setcollecting(true);
}

void Steps_OnRecordStop()
{
  if (!s_armed) return;
  if (s_paused && s_t0) s_pausedAcc += timeGetTime() - s_pauseStart;
  s_paused = false;
  s_durMs = steps_elapsed(timeGetTime());
  s_armed = false;
  stepsplat_setcollecting(false);
}

int Steps_Count() { return s_count; }

// ---------------------------------------------------------------- capture

static void steps_click_desc(const StepEvent *ev, const char *core,
                             char *out, int outsz)
{
  char mods[48];
  mods[0] = 0;
  if (ev->mods & kCGEventFlagMaskControl) strncat(mods, "Ctrl+", sizeof(mods) - strlen(mods) - 1);
  if (ev->mods & kCGEventFlagMaskShift)   strncat(mods, "Shift+", sizeof(mods) - strlen(mods) - 1);
  if (ev->mods & kCGEventFlagMaskAlternate) strncat(mods, "Opt+", sizeof(mods) - strlen(mods) - 1);
  if (ev->mods & kCGEventFlagMaskCommand) strncat(mods, "Cmd+", sizeof(mods) - strlen(mods) - 1);

  const char *verb = ev->kind == 2 ? "right-clicked" :
                     ev->kind == 3 ? "middle-clicked" : "clicked";
  snprintf(out, outsz, "%s%s %s", mods, verb, core);
}

static StepRec *steps_add(unsigned int t)
{
  if (s_count >= STEPS_MAX)
  {
    // drop the oldest step (and its thumbnail) to stay within budget
    if (s_steps[0].thumb[0]) unlink(s_steps[0].thumb);
    memmove(s_steps, s_steps + 1, sizeof(StepRec) * (STEPS_MAX - 1));
    s_count = STEPS_MAX - 1;
    ++s_truncated;
  }
  StepRec *s = s_steps + s_count;
  memset(s, 0, sizeof(StepRec));
  s->t = t;
  s->mx = s->my = -1;
  ++s_count;
  return s;
}

void Steps_OnFrame(LICE_IBitmap *bm, int capLeft, int capMinY, unsigned int now)
{
  StepEvent evs[STEPS_MAX_EVENTS];
  if (!s_armed || !bm) return;

  int n = stepsplat_drain(evs, STEPS_MAX_EVENTS);
  if (!s_t0) s_t0 = now; // timeline starts with the first captured frame
  if (!n || s_paused) return;

  const int bw = bm->getWidth(), bh = bm->getHeight();
  // replicate GetScreenData()'s rect origin: top-left of the captured region
  // in CG global display coords (same expression as capturewindow.mm uses),
  // so marker math stays consistent with whatever was actually captured.
  const int capTopY = (int)CGDisplayPixelsHigh(CGMainDisplayID()) - capMinY - bh;

  for (int i = 0; i < n; ++i)
  {
    const StepEvent &ev = evs[i];

    if (ev.kind == 4)
    {
      // merge keystroke bursts into a single "Typed N keystrokes" step
      if (s_count > 0 && s_steps[s_count - 1].isKey &&
          ev.t - s_lastKeyT < STEPS_KEY_MERGE_MS)
      {
        ++s_steps[s_count - 1].keyCount;
        s_lastKeyT = ev.t;
        continue;
      }

      char ctx[256];
      ctx[0] = 0;
      if (stepsplat_describe(&ev, NULL, 0, ctx, sizeof(ctx))) continue; // own app

      StepRec *s = steps_add(ev.t);
      s->isKey = true;
      s->keyCount = 1;
      snprintf(s->ctx, sizeof(s->ctx), "%s", ctx[0] ? ctx : "in an application");
      steps_make_thumb(bm, -1, -1, &s->mx, &s->my,
                       s->thumb, sizeof(s->thumb));
      s_lastKeyT = ev.t;
      continue;
    }

    // coalesce double-clicks (and rapid repeats on the same control)
    if (s_haveLastMouse && ev.kind == s_lastMouseKind &&
        ev.t - s_lastMouseT < STEPS_DBL_MS &&
        abs(ev.x - s_lastMouseX) <= STEPS_DBL_PX &&
        abs(ev.y - s_lastMouseY) <= STEPS_DBL_PX)
      continue;

    char core[512];
    core[0] = 0;
    if (stepsplat_describe(&ev, core, sizeof(core), NULL, 0)) continue; // own app

    StepRec *s = steps_add(ev.t);
    steps_click_desc(&ev, core[0] ? core : "", s->desc, sizeof(s->desc));

    // marker only when the click lands inside the captured region
    int mkx = ev.x - capLeft, mky = ev.y - capTopY;
    if (mkx < 0 || mkx >= bw || mky < 0 || mky >= bh) { mkx = -1; mky = -1; }
    steps_make_thumb(bm, mkx, mky, &s->mx, &s->my,
                     s->thumb, sizeof(s->thumb));

    s_lastMouseT = ev.t;
    s_lastMouseX = ev.x;
    s_lastMouseY = ev.y;
    s_lastMouseKind = ev.kind;
    s_haveLastMouse = true;
  }
}

// ----------------------------------------------------------------- export

static bool steps_file_base64(FILE *out, const char *path)
{
  FILE *in = fopen(path, "rb");
  if (!in) return false;
  if (fseek(in, 0, SEEK_END)) { fclose(in); return false; }
  const long n = ftell(in);
  if (n <= 0 || n > 4 * 1024 * 1024) { fclose(in); return false; }
  rewind(in);

  unsigned char *buf = (unsigned char *)malloc((size_t)n);
  if (!buf) { fclose(in); return false; }
  const size_t got = fread(buf, 1, (size_t)n, in);
  fclose(in);
  if (got != (size_t)n) { free(buf); return false; }

  const int b64n = (int)(((got + 2) / 3) * 4);
  char *b64 = (char *)malloc((size_t)b64n + 1);
  if (!b64) { free(buf); return false; }
  wdl_base64encode(buf, b64, (int)got);
  free(buf);

  bool ok = fwrite(b64, 1, (size_t)b64n, out) == (size_t)b64n;
  free(b64);
  return ok;
}

bool Steps_Export(const char *recordingFn, char *outPath, int outPathSz)
{
  if (outPath && outPathSz > 0) outPath[0] = 0;
  if (s_exported || s_count < 1 || !recordingFn || !recordingFn[0] ||
      !outPath || outPathSz < 16)
    return false;

  snprintf(outPath, outPathSz, "%s", recordingFn);
  char *sl = strrchr(outPath, '/');
  char *dot = strrchr(outPath, '.');
  if (dot && (!sl || dot > sl)) *dot = 0;
  const size_t len = strlen(outPath);
  if (len + 16 >= (size_t)outPathSz) { outPath[0] = 0; return false; }
  snprintf(outPath + len, (size_t)outPathSz - len, "_steps.html");

  FILE *f = fopen(outPath, "w");
  if (!f) return false;

  // header
  const char *base = strrchr(recordingFn, '/');
  base = base ? base + 1 : recordingFn;
  char datebuf[64] = "";
  {
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", &tmv);
  }
  char dur[32];
  steps_format_ms(s_durMs, dur, sizeof(dur));

  fputs("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>", f);
  html_escape(f, base);
  fputs(" &mdash; text guide</title>\n<style>\n"
        ":root { color-scheme: light dark; }\n"
        "body { font: 15px/1.5 -apple-system, BlinkMacSystemFont, \"Segoe UI\", Helvetica, Arial, sans-serif;\n"
        "       margin: 0; padding: 24px 16px 64px; background: #fafafa; color: #1c1c1e; }\n"
        "@media (prefers-color-scheme: dark) { body { background: #1c1c1e; color: #eee; }\n"
        "  .step { background: #2c2c2e !important; border-color: #444 !important; }\n"
        "  .meta, .foot { color: #aaa !important; } }\n"
        "main { max-width: 640px; margin: 0 auto; }\n"
        "h1 { font-size: 22px; margin: 0 0 4px; }\n"
        ".meta { color: #666; font-size: 13px; margin-bottom: 24px; }\n"
        "ol { list-style: none; margin: 0; padding: 0; counter-reset: step; }\n"
        ".step { background: #fff; border: 1px solid #e3e3e6; border-radius: 10px;\n"
        "        padding: 12px 14px; margin-bottom: 14px; counter-increment: step; }\n"
        ".step::before { content: counter(step); display: inline-block; min-width: 22px;\n"
        "  height: 22px; line-height: 22px; text-align: center; border-radius: 50%;\n"
        "  background: #e22828; color: #fff; font-size: 12px; font-weight: 600;\n"
        "  margin-right: 8px; vertical-align: 1px; }\n"
        ".t { color: #888; font-size: 12px; font-variant-numeric: tabular-nums; margin-left: 6px; }\n"
        ".desc { margin: 0; display: inline; }\n"
        ".step img { display: block; margin: 10px 0 0 30px; max-width: 100%;\n"
        "  border: 1px solid #d0d0d4; border-radius: 6px; height: auto; }\n"
        ".foot { max-width: 640px; margin: 24px auto 0; color: #999; font-size: 12px; }\n"
        "@media print { body { background: #fff; padding: 0; }\n"
        "  .step { border: 1px solid #ccc; break-inside: avoid; } }\n"
        "</style></head><body><main>\n<h1>Text guide</h1>\n<p class=\"meta\">",
        f);
  html_escape(f, base);
  char meta[512];
  snprintf(meta, sizeof(meta),
           " \xe2\x80\x94 %d step%s, %s duration \xe2\x80\x94 recorded %s with %s",
           s_count, s_count == 1 ? "" : "s", dur, datebuf, LICECAP_VERSION);
  html_escape(f, meta);
  fputs("</p>\n<ol>\n", f);

  // steps
  for (int i = 0; i < s_count; ++i)
  {
    const StepRec *s = s_steps + i;
    char tbuf[32];
    steps_format_ms(steps_elapsed(s->t), tbuf, sizeof(tbuf));

    fputs("<li class=\"step\"><p class=\"desc\">", f);
    if (s->isKey)
    {
      // render from ctx at export time (burst count can still grow)
      char kb[320];
      snprintf(kb, sizeof(kb), "Typed %d keystroke%s %s",
               s->keyCount, s->keyCount == 1 ? "" : "s", s->ctx);
      html_escape(f, kb);
    }
    else html_escape(f, s->desc);
    fprintf(f, " <span class=\"t\">%s</span></p>", tbuf);
    if (s->thumb[0])
    {
      fputs("<img alt=\"screenshot at this step\" src=\"data:image/png;base64,", f);
      if (!steps_file_base64(f, s->thumb)) fputs("broken", f);
      fputs("\">", f);
    }
    fputs("</li>\n", f);
  }

  fputs("</ol>\n", f);
  if (s_truncated > 0)
    fprintf(f, "<p class=\"foot\">Note: %d earlier step(s) were omitted (recording longer than %d steps).</p>\n",
            s_truncated, STEPS_MAX);
  fputs("<p class=\"foot\">Generated by LICEcap &mdash; keystroke contents are never recorded.</p>\n", f);
  fputs("</main></body></html>\n", f);

  const bool ok = !ferror(f);
  fclose(f);
  if (!ok) { unlink(outPath); outPath[0] = 0; return false; }

  s_exported = true;
  steps_cleanup_thumbs(); // thumbnails are embedded; free temp files
  return true;
}

#endif // LICECAP_STEPS

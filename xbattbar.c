/*
 * $Id: xbattbar.c,v 1.16.2.4 2001/02/02 05:25:29 suguru Exp $
 *
 * xbattbar: yet another battery watcher for X11
 */

/*
 * Copyright (c) 1998-2001 Suguru Yamaguchi <suguru@wide.ad.jp>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 */

static char *ReleaseVersion="1.4.2";

#include <sys/types.h>

#ifdef __NetBSD__
#define ENVSYSUNITNAMES
#include <sys/param.h>
#include <sys/envsys.h>
#include <paths.h>
#include <stdint.h>
#endif /* __NetBSD__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <err.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define PollingInterval 10	/* APM polling interval in sec */

#define DefaultFont "fixed"
#define DefaultFontH 14
#define DefaultFontW 7

/*
 * Global variables
 */

int ac_line = -1;               /* AC line status */
int battery_level = -1;         /* battery level */

unsigned long onin, onout;      /* indicator colors for AC online */
unsigned long offin, offout;    /* indicator colors for AC offline */

int elapsed_time = 0;           /* for battery remaining estimation */

/* indicator default colors */
char *ONIN_C   = "green";
char *ONOUT_C  = "olive drab";
char *OFFIN_C  = "blue";
char *OFFOUT_C = "red";

int alwaysontop = False;

int bi_interval = PollingInterval;  /* interval of polling APM */

Display *disp;
int scr;
Window win;
Atom wm_delete_window;
GC gc_fill, gc_text, gc_frame;
XEvent theEvent;
XFontStruct *fontp = NULL;

static unsigned long pix_bg = 0;
static unsigned long pix_fg = 0;

static const char *font_name = DefaultFont;
static char *wm_name   = "xbattbar";

static unsigned int win_w = 64, win_h = 16;
static int win_x = 0, win_y = 0;
static int have_x = 0, have_y = 0;

/* for tooltip to display status */
#define TIP_PAD_X	6
#define TIP_PAD_Y	4
#define TIP_FRAME_WIDTH	1
#define TIP_MSGLEN	128
#define TIP_DELAY	1000    /* ms */

static Window tip = (Window)0;
static int tip_mapped = 0;
static unsigned int tip_pad_x = TIP_PAD_X, tip_pad_y = TIP_PAD_Y;
static char tipmsg[TIP_MSGLEN];
static const int tip_delay_ms = TIP_DELAY;
static int tip_hovering = 0;
static struct timespec tip_disp = { 0 };
static int tip_xroot = 0, tip_yroot = 0;

/*
 * function prototypes
 */
void InitDisplay(void);
Status AllocColor(char *, unsigned long *);
void battery_check(void);
void redraw(void);
void usage(char **);
void about_this_program(void);
void estimate_remain(void);

static int pointer_in_window(Window);
static int pointer_in_windows(void);
static void tip_format(void);
static void tip_ensure_created(void);
static void tip_show(int root_x, int root_y);
static void tip_draw(void);
static void tip_hide(void);

/*
 * usage of this command
 */
void about_this_program()
{
  fprintf(stderr, 
	  "This is xbattbar version %s, "
	  "copyright (c)1998-2001 Suguru Yamaguchi\n",
	  ReleaseVersion);
}

void usage(char **argv)
{
  fprintf(stderr,
    "\n"	  
    "usage:\t%s [-h|v] [-g geometry] [-p sec]\n"
    "\t\t[-I color] [-O color] [-i color] [-o color] [-F font]\n"
    "-v, -h: show this message.\n"
    "-g:     set window geometry (WxH+X+Y).\n"
    "-p:     polling interval. [def: 10 sec.]\n"
    "-I, -O: bar colors in AC on-line. [def: \"green\" & \"olive drab\"]\n"
    "-i, -o: bar colors in AC off-line. [def: \"blue\" and \"red\"]\n"
    "-F:     font name. [def: \"fixed\"]\n",
	  argv[0]);
  exit(0);
}

/*
 * struct timespec helper functions
 */
static inline void
timespec_sub(struct timespec *tsp, struct timespec *usp, struct timespec *vsp)
{
  vsp->tv_sec = tsp->tv_sec - usp->tv_sec;
  vsp->tv_nsec = tsp->tv_nsec - usp->tv_nsec;
  if (vsp->tv_nsec < 0) {
    vsp->tv_sec--;
    vsp->tv_nsec += 1000000000;
  }
  if (vsp->tv_sec < 0) {
    /* return zero if (tsp < usp) */
    vsp->tv_sec = 0;
    vsp->tv_nsec = 0;
  }
}

static inline void
timespec_add_msec(struct timespec *tsp, time_t msec)
{
  tsp->tv_sec += msec / 1000;
  tsp->tv_nsec += (msec % 1000) * 1000000;
  if (tsp->tv_nsec >= 1000000000) {
    tsp->tv_sec++;
    tsp->tv_nsec -= 1000000000;
  }
}

static inline int
timespec_cmp(struct timespec *tsp, struct timespec *usp)
{
  if (tsp->tv_sec != usp->tv_sec) {
    if (tsp->tv_sec < usp->tv_sec) {
      return -1; /* tsp < usp */
    } else {
      return 1;  /* tsp > usp */
    }
  }
  if (tsp->tv_nsec != usp->tv_nsec) {
    if (tsp->tv_nsec < usp->tv_nsec) {
      return -1; /* tsp < usp */
    } else {
      return 1;  /* tsp > usp */
    }
  }
  return 0;      /* tsp == usp */
}

/*
 * AllocColor:
 * convert color name to pixel value
 */
Status AllocColor(char *name, unsigned long *pixel)
{
  XColor color,exact;
  int status;

  status = XAllocNamedColor(disp, DefaultColormap(disp, scr),
                           name, &color, &exact);
  *pixel = color.pixel;

  return(status);
}

/*
 * InitDisplay:
 * create a window for WM Swallow
 */
void InitDisplay(void)
{

  if((disp = XOpenDisplay(NULL)) == NULL) {
      fprintf(stderr, "xbattbar: can't open display.\n");
      exit(1);
  }
  scr = DefaultScreen(disp);
  pix_bg = WhitePixel(disp, scr);
  pix_fg = BlackPixel(disp, scr);

  if (!AllocColor(ONIN_C,&onin) ||
       !AllocColor(OFFOUT_C,&offout) ||
       !AllocColor(OFFIN_C,&offin) ||
       !AllocColor(ONOUT_C,&onout)) {
    fprintf(stderr, "xbattbar: can't allocate color resources\n");
    exit(EXIT_FAILURE);
  }

  XSetWindowAttributes attr = {0};
  attr.background_pixel = pix_bg;
  attr.event_mask = ExposureMask | StructureNotifyMask |
    EnterWindowMask | LeaveWindowMask | PointerMotionMask;

  win = XCreateWindow(disp, RootWindow(disp, scr),
                      have_x ? win_x : 0, have_y ? win_y : 0,
                      win_w, win_h,
                      0,
                      DefaultDepth(disp, scr),
                      InputOutput,
                      DefaultVisual(disp, scr),
                      CWBackPixel | CWEventMask,
                      &attr
    );

  /* set WM_NAME / CLASS for WM Swallow */
  XStoreName(disp, win, wm_name);
  XClassHint ch;
  ch.res_name  = wm_name;
  ch.res_class = (char *)"Xbattbar";
  XSetClassHint(disp, win, &ch);

  gc_fill  = XCreateGC(disp, win, 0, NULL);
  gc_frame = XCreateGC(disp, win, 0, NULL);

  fontp = XLoadQueryFont(disp, font_name);
  if (fontp == NULL)
    fontp = XLoadQueryFont(disp, "fixed");
  if (fontp != NULL) {
    XGCValues gv = {0};
    gv.font = fontp->fid;
    gc_text = XCreateGC(disp, win, GCFont, &gv);
  }

  XMapWindow(disp, win);

  wm_delete_window = XInternAtom(disp, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(disp, win, &wm_delete_window, 1);
}

int main(int argc, char **argv)
{
  int ch;
  char *geom = NULL;
  Atom proto;
  struct timespec next;
  int xfd;

  about_this_program();
  while ((ch = getopt(argc, argv, "g:F:hI:i:O:o:p:v")) != -1)
    switch (ch) {
    case 'I':
      ONIN_C = optarg;
      break;
    case 'i':
      OFFIN_C = optarg;
      break;
    case 'O':
      ONOUT_C = optarg;
      break;
    case 'o':
      OFFOUT_C = optarg;
      break;

    case 'F':
      font_name = optarg;
      break;

    case 'g':
      geom = optarg;
      break;

    case 'p':
      bi_interval = atoi(optarg);
      break;

    case 'h':
    case 'v':
      usage(argv);
      break;
    }

  if (geom) {
    int x, y;
    unsigned int w, h;
    int flags = XParseGeometry(geom, &x, &y, &w, &h);
    if (flags & WidthValue) {
      win_w = w;
    }
    if (flags & HeightValue) {
      win_h = h;
    }
    if (flags & XValue) {
      have_x = 1;
      win_x = x;
    }
    if (flags & YValue) {
      have_y = 1;
      win_y = y;
    }
  }

  /*
   * X Window main loop
   */
  InitDisplay();
  battery_check();
  clock_gettime(CLOCK_MONOTONIC, &next);
  timespec_add_msec(&next, (time_t)bi_interval * 1000);
  xfd = ConnectionNumber(disp);
  proto = XInternAtom(disp, "WM_PROTOCOLS", False);
  while (1) {
    fd_set fds;
    struct timespec now, wait, hoverwait;
    struct timeval tv;
    int rv;

    FD_ZERO(&fds);
    FD_SET(xfd, &fds);

    /* Calculate wait time to poll the next battery status */
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespec_sub(&next, &now, &wait);

    /* Calculate wait time for delayed tooltip */
    if (tip_hovering && !tip_mapped) {
      timespec_sub(&tip_disp, &now, &hoverwait);
      if (timespec_cmp(&wait, &hoverwait) > 0) {
        wait = hoverwait;
      }
    }

    tv.tv_sec = wait.tv_sec;
    tv.tv_usec = wait.tv_nsec / 1000;
    rv = select(xfd + 1, &fds, NULL, NULL, &tv);
    if (rv < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("select");
      exit(EXIT_FAILURE);
    }
    if (rv > 0 && FD_ISSET(xfd, &fds)) {
      while (XPending(disp) > 0) {
        XNextEvent(disp, &theEvent);
        switch (theEvent.type) {
        case Expose:
          if (theEvent.xexpose.window == win) {
            redraw();
          } else if (theEvent.xexpose.window == tip) {
            tip_draw();
          }
          break;
        case ConfigureNotify:
          redraw();
          break;

        case EnterNotify:
          if (theEvent.xcrossing.window == win) {
            tip_hovering = 1;
            clock_gettime(CLOCK_MONOTONIC, &tip_disp);
            timespec_add_msec(&tip_disp, tip_delay_ms);
            tip_xroot = theEvent.xcrossing.x_root;
            tip_yroot = theEvent.xcrossing.y_root;
          }
          break;
        case LeaveNotify:
          if (theEvent.xcrossing.window == win) {
            tip_hovering = 0;
            if (!pointer_in_windows()) {
              tip_hide();
            }
          } else if (theEvent.xcrossing.window == tip) {
            if (!pointer_in_windows()) {
              tip_hide();
            }
          }
          break;
        case MotionNotify:
          tip_xroot = theEvent.xmotion.x_root;
          tip_yroot = theEvent.xmotion.y_root;
          if (tip_mapped) {
            tip_show(tip_xroot, tip_yroot);
          }
          break;

        case ClientMessage:
          if (theEvent.xclient.message_type == proto &&
              (Atom)theEvent.xclient.data.l[0] == wm_delete_window) {
            goto out;
          }
        }
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (timespec_cmp(&now, &next) >= 0) {
      battery_check();
      while (timespec_cmp(&now, &next) >= 0) {
        timespec_add_msec(&next, bi_interval * 1000);
      }
    }
    if (tip_hovering && !tip_mapped) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (timespec_cmp(&now, &tip_disp) > 0) {
        tip_show(tip_xroot, tip_yroot);
      }
    }
  }

 out:
  exit(EXIT_SUCCESS);
}

static void draw_widget(void)
{
  XWindowAttributes wa;
  unsigned int width, height, margin, bx, by, bw, bh, fill_w;
  unsigned int pct;
  unsigned long col_in, col_out;

  XGetWindowAttributes(disp, win, &wa);
  width = wa.width;
  height = wa.height;

  /* background (white) */
  XSetForeground(disp, gc_fill, pix_bg);
  XFillRectangle(disp, win, gc_fill, 0, 0, width, height);

  /* frame (black) */
  margin = (width < 32 || height < 12) ? 1u : 2u;
  bx = by = margin;
  bw = (width > margin * 2U) ? (width - margin * 2U) : width;
  bh = (height > margin * 2U) ? (height - margin * 2U) : height;

  XSetForeground(disp, gc_frame, pix_fg);
  if (bw > 1U && bh > 1U)
    XDrawRectangle(disp, win, gc_frame, bx, by, bw - 1, bh - 1);

  /* draw battery capacity */
  pct = (battery_level < 0) ? 0U :
    (battery_level > 100 ? 100U : (unsigned int)battery_level);
  col_in  = ac_line ? onin  : offin;
  col_out = ac_line ? onout : offout;

  if (bw > 2U && bh > 2U) {
    XSetForeground(disp, gc_fill, col_out);
    XFillRectangle(disp, win, gc_fill, bx + 1U, by + 1U, bw - 2U, bh - 2U);

    fill_w = (bw - 2U) * pct / 100U;
    XSetForeground(disp, gc_fill, col_in);
    if (fill_w > 0U)
      XFillRectangle(disp, win, gc_fill, bx + 1U, by + 1U, fill_w, bh - 2U);
  }

  /* capacity percentage */
  if (fontp != NULL && gc_text != 0) {
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "%u%%", pct);
    int tw = XTextWidth(fontp, buf, len);
    int tx = (int)(width - tw) / 2;
    int ty = (int)(height + fontp->ascent - fontp->descent) / 2;

    XSetForeground(disp, gc_text, pix_bg);
    XDrawString(disp, win, gc_text, tx + 1, ty + 1, buf, len);
    XSetForeground(disp, gc_text, pix_fg);
    XDrawString(disp, win, gc_text, tx, ty, buf, len);
  }

  XFlush(disp);
}

void redraw(void)
{
  draw_widget();
  estimate_remain();
  if (tip_mapped) {
    tip_format();
    tip_draw();
  }
}

/*
 * tooltip to display status
 */

static int pointer_in_window(Window window)
{
  Window root_ret, child_ret;
  int rx, ry, wx, wy;
  unsigned int mask;
  XWindowAttributes wa;

  if (window == 0) {
    return 0;
  }
  if (!XGetWindowAttributes(disp, window, &wa)) {
    return 0;
  }

  if (!XQueryPointer(disp, window, &root_ret, &child_ret,
    &rx, &ry, &wx, &wy, &mask)) {
    return 0;
  }

  return wx >= 0 && wy >= 0 && wx < (int)wa.width && wy < (int)wa.height;
}

static int pointer_in_windows(void)
{
  if (pointer_in_window(win)) {
    return 1;
  }
  if (tip_mapped && pointer_in_window(tip)) {
    return 1;
  }
  return 0;
}

static void tip_format(void)
{
  snprintf(tipmsg, sizeof(tipmsg),
           "AC %s-line: battery level is %d%%",
           ac_line ? "on" : "off", battery_level);
}

static void tip_ensure_created(void)
{
  if (tip)
    return;
  tip = XCreateSimpleWindow(
          disp, RootWindow(disp, scr),
          0, 0, 1, 1,               /* pos and size will be updated later */
          TIP_FRAME_WIDTH,          /* width of frame */
          pix_fg, pix_bg);
  XSetWindowAttributes swa;
  swa.override_redirect = True;     /* exclude from WM */
  XChangeWindowAttributes(disp, tip, CWOverrideRedirect, &swa);
  XSelectInput(disp, tip, ExposureMask |
    EnterWindowMask | LeaveWindowMask | PointerMotionMask);
}

static void tip_draw(void)
{
  if (!tip_mapped)
    return;

  XWindowAttributes wa;
  XGetWindowAttributes(disp, tip, &wa);
  unsigned int width = wa.width, height = wa.height;

  /* background and frame */
  XSetForeground(disp, gc_fill, pix_bg);
  XFillRectangle(disp, tip, gc_fill, 0, 0, width, height);
  XSetForeground(disp, gc_frame, pix_fg);
  XDrawRectangle(disp, tip, gc_frame, 0, 0, width - 1, height - 1);

  /* status strings */
  if (fontp != NULL && gc_text != 0) {
    int ty = (int)tip_pad_y + fontp->ascent;
    int tx = (int)tip_pad_x;

    XSetForeground(disp, gc_text, pix_fg);
    XDrawString(disp, tip, gc_text, tx, ty, tipmsg, strlen(tipmsg));
  }

  XFlush(disp);
}

static void tip_show(int root_x, int root_y)
{
  int tw, th, x, y, sw, sh;
  unsigned int width, height;
  int len;

  tip_ensure_created();
  tip_format();

  /* Calculate window size */
  len = strlen(tipmsg);
  if (fontp != NULL) {
    tw = XTextWidth(fontp, tipmsg, len);
    th = fontp->ascent + fontp->descent;
  } else {
    tw = DefaultFontW * len;
    th = DefaultFontH;
  }
  width = (unsigned int)(tw + tip_pad_x * 2);
  height = (unsigned int)(th + tip_pad_y * 2);

  /* Adjust window location */
  x = root_x + 8;
  y = root_y - height - (TIP_FRAME_WIDTH * 2 + 2);
  sw = DisplayWidth(disp, scr);
  sh = DisplayHeight(disp, scr);
  if (x + (int)width > sw)
    x = sw - (int)width;
  if (y + (int)height > sh)
    y = sh - (int)height;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;

  XMoveResizeWindow(disp, tip, x, y, width, height);
  if (!tip_mapped) {
    XMapRaised(disp, tip);
    tip_mapped = 1;
  }
  tip_draw();
}

static void tip_hide(void)
{
  if (!tip)
    return;
  XUnmapWindow(disp, tip);
  tip_mapped = 0;
}

/*
 * estimating time for battery remaining / charging 
 */

#define CriticalLevel  5

void estimate_remain()
{
  static int battery_base = -1;
  int diff;
  int remain;

  /* static value initialize */
  if (battery_base == -1) {
    battery_base = battery_level;
    return;
  }

  diff = battery_base - battery_level;

  if (diff == 0) return;

  /* estimated time for battery remains */
  if (diff > 0) {
    remain = elapsed_time * (battery_level - CriticalLevel) / diff ;
    remain = remain * bi_interval;  /* in sec */
    if (remain < 0 ) remain = 0;
    printf("battery remain: %2d hr. %2d min. %2d sec.\n",
	   remain / 3600, (remain % 3600) / 60, remain % 60);
    elapsed_time = 0;
    battery_base = battery_level;
    return;
  }

  /* estimated time of battery charging */
  remain = elapsed_time * (battery_level - 100) / diff;
  remain = remain * bi_interval;  /* in sec */
  printf("charging remain: %2d hr. %2d min. %2d sec.\n",
	 remain / 3600, (remain % 3600) / 60, remain % 60);
  elapsed_time = 0;
  battery_base = battery_level;
}



#ifdef __bsdi__

#include <machine/apm.h>
#include <machine/apmioctl.h>

int first = 1;
void battery_check(void)
{
  int fd;
  struct apmreq ar ;

  ++elapsed_time;

  ar.func = APM_GET_POWER_STATUS ;
  ar.dev = APM_DEV_ALL ;

  if ((fd = open(_PATH_DEVAPM, O_RDONLY)) < 0) {
    perror(_PATH_DEVAPM) ;
    exit(1) ;
  }
  if (ioctl(fd, PIOCAPMREQ, &ar) < 0) {
    fprintf(stderr, "xbattbar: PIOCAPMREQ: APM_GET_POWER_STATUS error 0x%x\n", ar.err);
  }
  close (fd);

  if (first || ac_line != ((ar.bret >> 8) & 0xff)
      || battery_level != (ar.cret&0xff)) {
    first = 0;
    ac_line = (ar.bret >> 8) & 0xff;
    battery_level = ar.cret&0xff;
    redraw();
  }
}

#endif /* __bsdi__ */

#ifdef __FreeBSD__

#include <machine/apm_bios.h>

#define APMDEV21       "/dev/apm0"
#define APMDEV22       "/dev/apm"

#define        APM_STAT_UNKNOWN        255

#define        APM_STAT_LINE_OFF       0
#define        APM_STAT_LINE_ON        1

#define        APM_STAT_BATT_HIGH      0
#define        APM_STAT_BATT_LOW       1
#define        APM_STAT_BATT_CRITICAL  2
#define        APM_STAT_BATT_CHARGING  3

int first = 1;
void battery_check(void)
{
  int fd, r, p;
  struct apm_info     info;

  if ((fd = open(APMDEV21, O_RDWR)) == -1 &&
      (fd = open(APMDEV22, O_RDWR)) == -1) {
    fprintf(stderr, "xbattbar: cannot open apm device\n");
    exit(1);
  }
  if (ioctl(fd, APMIO_GETINFO, &info) == -1) {
    fprintf(stderr, "xbattbar: ioctl APMIO_GETINFO failed\n");
    exit(1);
  }
  close (fd);

  ++elapsed_time;

  /* get current status */
  if (info.ai_batt_life == APM_STAT_UNKNOWN) {
    switch (info.ai_batt_stat) {
    case APM_STAT_BATT_HIGH:
      r = 100;
      break;
    case APM_STAT_BATT_LOW:
      r = 40;
      break;
    case APM_STAT_BATT_CRITICAL:
      r = 10;
      break;
    default:        /* expected to be APM_STAT_UNKNOWN */
      r = 100;
    }
  } else if (info.ai_batt_life > 100) {
    /* some APM BIOSes return values slightly > 100 */
    r = 100;
  } else {
    r = info.ai_batt_life;
  }

  /* get AC-line status */
  if (info.ai_acline == APM_STAT_LINE_ON) {
    p = APM_STAT_LINE_ON;
  } else {
    p = APM_STAT_LINE_OFF;
  }

  if (first || ac_line != p || battery_level != r) {
    first = 0;
    ac_line = p;
    battery_level = r;
    redraw();
  }
}

#endif /* __FreeBSD__ */

#ifdef __NetBSD__

#ifndef _NO_APM
#include <machine/apmvar.h>
#else
#define APM_AC_OFF	0x00
#define APM_AC_ON	0x01
#endif

#define _PATH_APM_SOCKET       "/var/run/apmdev"
#define _PATH_APM_CTLDEV       "/dev/apmctl"
#define _PATH_APM_NORMAL       "/dev/apm"

/*
 * pre:  fd contains a valid file descriptor of an envsys(4) supporting device
 *       && ns is the number of sensors
 *       && etds and ebis are arrays of sufficient size
 * post: returns 0 and etds and ebis arrays are filled with sensor info
 *       or returns -1 on failure
 */
static int
fillsensors(int fd, envsys_tre_data_t *etds, envsys_basic_info_t *ebis,
    size_t ns)
{
	int i;

	for (i = 0; i < ns; ++i) {
		ebis[i].sensor = i;
		if (ioctl(fd, ENVSYS_GTREINFO, &ebis[i]) == -1) {
			warn("Can't get sensor info for sensor %d", i);
			return 0;
		}

		etds[i].sensor = i;
		if (ioctl(fd, ENVSYS_GTREDATA, &etds[i]) == -1) {
			warn("Can't get sensor data for sensor %d", i);
			return 0;
		}
	}
	return 1;
}

/*
 * pre:  fd contains a valid file descriptor of an envsys(4) supporting device
 * post: returns the number of valid sensors provided by the device
 *       or -1 on error
 */
static size_t
numsensors(int fd)
{
	int count = 0, valid = 1;
	envsys_tre_data_t etd;
	etd.sensor = 0;

	while (valid) {
		if (ioctl(fd, ENVSYS_GTREDATA, &etd) == -1)
			err(1, "Can't get sensor data");

		valid = etd.validflags & ENVSYS_FVALID;
		if (valid)
			++count;

		++etd.sensor;
	}

	return count;
}

static envsys_tre_data_t *etds;
static envsys_basic_info_t *ebis;
static int *cetds;

#if defined(_PATH_SYSMON) && __NetBSD_Version__ >= 106110000
#define HAVE_NETBSD_ACPI
#endif

int first = 1;
void battery_check(void)
{
       int fd, r, p;
#ifndef _NO_APM
       struct apm_power_info info;
#endif
       int acpi;
       size_t ns;
       size_t cc;
       char *apmdev;
       int i;

       acpi = 0;
       apmdev = _PATH_APM_NORMAL;
       if ((fd = open(apmdev, O_RDONLY)) == -1) {
#ifdef HAVE_NETBSD_ACPI
	       apmdev = _PATH_SYSMON;
	       fd = open(apmdev, O_RDONLY);
	       acpi = 1;
#endif
       }
       if (fd < 0) {
               fprintf(stderr, "xbattbar: cannot open %s device\n", apmdev);
               exit(1);
       }

       if (acpi) {
#ifdef HAVE_NETBSD_ACPI
		if ((ns = numsensors(fd)) == 0) {
		       fprintf(stderr, "xbattbar: no sensors found\n");
               exit(1);
       }
		if (first) {
			cetds = (int *)malloc(ns * sizeof(int));
			etds = (envsys_tre_data_t *)malloc(ns * sizeof(envsys_tre_data_t));
			ebis = (envsys_basic_info_t *)malloc(ns * sizeof(envsys_basic_info_t));

			if ((cetds == NULL) || (etds == NULL) || (ebis == NULL)) {
				err(1, "Out of memory");
			}
		}

		fillsensors(fd, etds, ebis, ns);

#endif
#ifndef _NO_APM
       } else {

	       memset(&info, 0, sizeof(info));
       if (ioctl(fd, APM_IOC_GETPOWER, &info) != 0) {
               fprintf(stderr, "xbattbar: ioctl APM_IOC_GETPOWER failed\n");
               exit(1);
       }
#endif
       }

       close(fd);

       ++elapsed_time;

       if (acpi) {
#ifdef HAVE_NETBSD_ACPI
		int32_t rtot = 0, maxtot = 0;
		int have_pct = 0;
		p = APM_AC_ON;
		for (i = 0 ; i < ns ; i++) {
			if ((etds[i].validflags & ENVSYS_FCURVALID) == 0)
				continue;
			cc = strlen(ebis[i].desc);
			if (strncmp(ebis[i].desc, "acpibat", 7) == 0 &&
			    (strcmp(&ebis[i].desc[cc - 7], " charge") == 0 ||
			     strcmp(&ebis[i].desc[cc - 7], " energy") == 0)) {
				rtot += etds[i].cur.data_s;
				maxtot += etds[i].max.data_s;
			}
			/*
			 * XXX: We should use acpiacad driver and look for
			 * " connected", but that's broken on some machines
			 * and we want this to work everywhere.  With this
			 * we will occasionally catch a machine conditioning
			 * a battery while connected, while other machines take
			 * 10-15 seconds to switch from "charging" to
			 * "discharging" and vice versa, but this is the best
			 * compromise.
			 */
			if ((ebis[i].units == ENVSYS_SWATTS || ebis[i].units == ENVSYS_SAMPS) &&
			    etds[i].cur.data_s &&
			    strncmp(ebis[i].desc, "acpibat", 7) == 0 &&
			    strcmp(&ebis[i].desc[cc - 14], "discharge rate") == 0) {
				p = APM_AC_OFF;
			}

			if (ebis[i].units == ENVSYS_INTEGER &&
			    strcmp(ebis[i].desc, "battery percent") == 0) {
				have_pct = 1;
				r = etds[i].cur.data_s;
			}
			if (ebis[i].units == ENVSYS_INDICATOR &&
			    strcmp(ebis[i].desc, "ACIN present") == 0 &&
			    etds[i].cur.data_s == 0) {
				p = APM_AC_OFF;
			}
		}
		if (!have_pct)
			r = (rtot * 100.0) / maxtot;
#endif
#ifndef _NO_APM
       } else {
	       /* get current remain */
       if (info.battery_life > 100) {
               /* some APM BIOSes return values slightly > 100 */
               r = 100;
       } else {
               r = info.battery_life;
       }

       /* get AC-line status */
       if (info.ac_state == APM_AC_ON) {
               p = APM_AC_ON;
       } else {
               p = APM_AC_OFF;
       }
#endif
       }

       if (first || ac_line != p || battery_level != r) {
               first = 0;
               ac_line = p;
               battery_level = r;
               redraw();
       }
}

#endif /* __NetBSD__ */


#ifdef linux

#include <errno.h>
#include <linux/apm_bios.h>

#define APM_PROC	"/proc/apm"

#define        APM_STAT_LINE_OFF       0
#define        APM_STAT_LINE_ON        1

typedef struct apm_info {
   char       driver_version[16];
   int        apm_version_major;
   int        apm_version_minor;
   int        apm_flags;
   int        ac_line_status;
   int        battery_status;
   int        battery_flags;
   int        battery_percentage;
   int        battery_time;
   int        using_minutes;
} apm_info;


int first = 1;
void battery_check(void)
{
  int r,p;
  FILE *pt;
  struct apm_info i;
  char buf[100];

  /* get current status */
  errno = 0;
  if ( (pt = fopen( APM_PROC, "r" )) == NULL) {
    fprintf(stderr, "xbattbar: Can't read proc info: %s\n", strerror(errno));
    exit(1);
  }

  fgets( buf, sizeof( buf ) - 1, pt );
  buf[ sizeof( buf ) - 1 ] = '\0';
  sscanf( buf, "%15s %d.%d %x %x %x %x %d%% %d %d\n",
	  i.driver_version,
	  &i.apm_version_major,
	  &i.apm_version_minor,
	  &i.apm_flags,
	  &i.ac_line_status,
	  &i.battery_status,
	  &i.battery_flags,
	  &i.battery_percentage,
	  &i.battery_time,
	  &i.using_minutes );

  fclose (pt);

  ++elapsed_time;

   /* some APM BIOSes return values slightly > 100 */
   if ( (r = i.battery_percentage) > 100 ){
     r = 100;
   }

   /* get AC-line status */
   if ( i.ac_line_status == APM_STAT_LINE_ON) {
     p = APM_STAT_LINE_ON;
   } else {
     p = APM_STAT_LINE_OFF;
   }

  if (first || ac_line != p || battery_level != r) {
    first = 0;
    ac_line = p;
    battery_level = r;
    redraw();
  }
}

#endif /* linux */


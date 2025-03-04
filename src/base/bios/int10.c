/*
 * DANG_BEGIN_MODULE
 *
 * REMARK
 * Video BIOS implementation.
 *
 * This module handles the int10 video functions.
 * Most functions here change only the video memory and status
 * variables; the actual screen is then rendered asynchronously
 * after these by Video->update_screen.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * DANG_BEGIN_CHANGELOG
 *
 * 5/24/95, Erik Mouw (J.A.K.Mouw@et.tudelft.nl) and
 * Arjan Filius (I.A.Filius@et.tudelft.nl)
 * changed int10() to make graphics work with X.
 *
 * 1998/04/05: Put some work into set_video_mode() (made it
 * more VGA compatible) and removed new_set_video_mode().
 * Removed (useless) global variable "gfx_mode".
 * -- sw (Steffen Winterfeldt <wfeldt@suse.de>)
 *
 * Readded new_set_video_mode, its needed for non-X compiles.
 * -- EB 3 June 1998
 *
 * Renamed new_set_video_mode to X_set_video_mode to avoid confusion
 * in the future
 * -- Hans 980614
 *
 * 1998/12/12: Fixed some bugs and integrated Josef Pavlik's <jet@spintec.com>
 * patches; improved font/palette changes, get screen mode (ah = 0x0f) fixed,
 * cursor shape is now initialized during mode set.
 * -- sw
 *
 * 2000/05/18: Split int10() into a X and non-X part. Reworked to X part so
 * that it supports fonts in gfx modes.
 * -- sw
 *
 * 2002/11/30: Started user font support. Needs some more work!
 * -- eric@coli.uni-sb.de - Eric
 *
 * DANG_END_CHANGELOG
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#include "emu.h"
#include "video.h"
#include "bios.h"
#include "int.h"
#include "port.h"
#include "speaker.h"
#include "utilities.h"
#include "dos2linux.h"
#include "timers.h"
#include "vgaemu.h"
#include "vgatext.h"

/*
 * Activate some debug output.
 */
#define DEBUG_INT10	1
#define BIOS_CONFIG_SCREEN_MODE (READ_WORD(BIOS_CONFIGURATION) & 0x30)
#define IS_SCREENMODE_MDA (BIOS_CONFIG_SCREEN_MODE == 0x30)

static const int max_page = 7;
int video_mode;
int video_combo;

unsigned screen_adr(int page)
{
  /* This is the text screen base, the DOS program actually has to use.
   * Programs that support simultaneous dual monitor support rely on
   * the fact, that the BIOS takes B0000 for EQUIP-flags 4..5 = 3
   * else B8000 as regenbuffer address. Each compatible PC-BIOS behaves so.
   * This is ugly, but there is no screen buffer address in the BIOS-DATA
   * at 0x400. (Hans)
   */
  unsigned base = IS_SCREENMODE_MDA ? MDA_VIRT_TEXT_BASE : VGA_VIRT_TEXT_BASE;
  return base + page * READ_WORD(BIOS_VIDEO_MEMORY_USED);
}

/* this maps the cursor shape given by int10, fn1 to the actually
   displayed cursor start&end values in cursor_shape. This seems
   to be typical IBM Black Compatibility Magic.
   I modeled it approximately from the behaviour of my own
   VGA's BIOS.
   I'm not sure if it is correct for start=end and for font_heights
   other than 16.
*/

#define i10_msg(x...) v_printf("INT10: " x)

#if DEBUG_INT10 >= 1
#define i10_deb(x...) v_printf("INT10: " x)
#else
#define i10_deb(x...)
#endif

static void tty_char_out(unsigned char ch, int s, int attr);
static void vga_ROM_to_RAM(unsigned height, int bank);

static void crt_outw(unsigned index, unsigned value)
{
  unsigned port = READ_WORD(BIOS_VIDEO_PORT);
  port_outw(port, index | (value & 0xff00));
  port_outw(port, (index + 1) | ((value & 0xff) << 8));
}

static unsigned do_set_cursor_pos(unsigned page, int x, int y)
{
  unsigned co, old_y;

  old_y = get_bios_cursor_y_position(page);
  set_bios_cursor_x_position(page, x);
  set_bios_cursor_y_position(page, y);
  co = READ_WORD(BIOS_SCREEN_COLUMNS);
  crt_outw(0xe, READ_WORD(BIOS_VIDEO_MEMORY_ADDRESS)/2 + y * co + x);
  return old_y;
}

static void set_cursor_pos(unsigned page, int x, int y)
{
  unsigned old_y = do_set_cursor_pos(page, x, y);

  if (config.dumb_video && y > old_y) {
    int i;

    if (no_local_video && !config.tty_stderr)
      return;
    for (i = 0; i < y - old_y; i++)
      fputs("\r\n", config.tty_stderr ? stderr : stdout);
  }
}

static void set_cursor_shape(ushort shape)
{
   int cs,ce;
   cshape cursor_shape;
   cursor_shape.w = shape;

   WRITE_WORD(BIOS_CURSOR_SHAPE, cursor_shape.w);

   cs=CURSOR_START(cursor_shape) & 0x1F;
   ce=CURSOR_END(cursor_shape) & 0x1F;

   if (cursor_shape.w & 0x6000 || cs>ce) {
      i10_deb("no cursor\n");
      crt_outw(0xa, NO_CURSOR);
      return;
   }

   cs&=0x0F;
   ce&=0x0F;
   if (ce>3 && ce<12 && (config.cardtype != CARD_MDA)) {
      int vga_font_height = READ_WORD(BIOS_FONT_HEIGHT);
      if (cs>ce-3) cs+=vga_font_height-ce-1;
      else if (cs>3) cs=vga_font_height/2;
      ce=vga_font_height-1;
   }
   i10_msg("mapped cursor: start %d, end %d\n", cs, ce);
   CURSOR_START(cursor_shape)=cs;
   CURSOR_END(cursor_shape)=ce;
   crt_outw(0xa, cursor_shape.w);
}

/* This is a better scroll routine, mostly for aesthetic reasons. It was
 * just too horrible to contemplate a scroll that worked 1 character at a
 * time :-)
 *
 * It may give some performance improvement on some systems (it does
 * on mine) (Andrew Tridgell)
 */
static void
bios_scroll(int x0, int y0, int x1, int y1, int l, int att)
{
  int dx = x1 - x0 + 1;
  int dy = y1 - y0 + 1;
  int x, y, co, li;
  uint16_t blank = ' ' | (att << 8);
  uint16_t tbuf[MAX_COLUMNS];
  unsigned sadr;

  if (config.dumb_video)
     return;

  li= READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1;
  co= READ_WORD(BIOS_SCREEN_COLUMNS);
  sadr = screen_adr(0) + READ_WORD(BIOS_VIDEO_MEMORY_ADDRESS);

  if (sadr < VGA_PHYS_TEXT_BASE && ((att & 7) != 0) && ((att & 7) != 7))
    {
      blank = ' ' | ((att | 7) << 8);
    }


  if (x1 >= co || y1 >= li)
    {
      v_printf("VID: Scroll parameters out of bounds, in Scroll!\n");
      v_printf("VID: Attempting to fix with clipping!\n");
    /* kludge for ansi.sys' clear screen - we'd better do real clipping */
    /* Also a cludge to fix list, but in the other dimension */
      if (x1 >= co) x1 = co -1;
      if (y1 >= li) y1 = li -1;
      dx = x1 - x0 +1;
      dy = y1 - x0 +1;
    }
  if (dx <= 0 || dy <= 0 || x0 < 0 || x1 >= co || y0 < 0 || y1 >= li)
    {
      v_printf("VID: Scroll parameters impossibly out of bounds, giving up!\n");
    return;
    }

  /* make a blank line */
  for (x = 0; x < dx; x++)
    tbuf[x] = blank;

  if (l >= dy || l <= -dy)
    l = 0;

  if (l == 0) {			/* Wipe mode */
    for (y = y0; y <= y1; y++)
      memcpy_to_vga(sadr + 2 * (y * co + x0), tbuf, dx * 2);
    return;
  }

  if (l > 0) {
    if (dx == co)
      vga_memcpy(sadr + 2 * y0 * co, sadr + 2 * (y0 + l) * co, (dy - l) * dx * 2);
    else
      for (y = y0; y <= (y1 - l); y++)
	vga_memcpy(sadr + 2 * (y * co + x0), sadr + 2 * ((y + l) * co + x0), dx * 2);

    for (y = y1 - l + 1; y <= y1; y++)
      memcpy_to_vga(sadr + 2 * (y * co + x0), tbuf, dx * 2);
  }
  else {
    for (y = y1; y >= (y0 - l); y--)
      vga_memcpy(sadr + 2 * (y * co + x0), sadr + 2 * ((y + l) * co + x0), dx * 2);

    for (y = y0 - l - 1; y >= y0; y--)
      memcpy_to_vga(sadr + 2 * (y * co + x0), tbuf, dx * 2);
  }
}

static int using_text_mode(void)
{
  unsigned char mode = READ_BYTE(BIOS_VDU_CONTROL);
  return (!(mode & 2));
}

static int using_mono_mode(void)
{
  return ((READ_BYTE(BIOS_VDU_CONTROL) & 0xc) == 0xc);
}

/* Output a character to the screen. */
void char_out(unsigned char ch, int page)
{
  tty_char_out(ch, page, -1);
}

/*
 * Output a character to the screen.
 * If attr != -1, set the attribute byte, too.
 */
void tty_char_out(unsigned char ch, int s, int attr)
{
  int xpos, ypos, co, li;
  int gfx_mode = 0;
  unsigned dst;

/* i10_deb("tty_char_out: char 0x%02x, page %d, attr 0x%02x\n", ch, s, attr); */

  if (config.dumb_video) {
    struct char_set_state term_state;
    t_unicode uni = dos_to_unicode_table[ch];
    unsigned char buff[MB_LEN_MAX + 1];
    int num, i;

    if (no_local_video && !config.tty_stderr)
      return;

    init_charset_state(&term_state, trconfig.output_charset);
    num = unicode_to_charset(&term_state, uni, buff, MB_LEN_MAX);
    if (num <= 0)
      return;
    for (i = 0; i < num; i++)
      fputc(buff[i], config.tty_stderr ? stderr : stdout);
  }

  li= READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1;
  co= READ_WORD(BIOS_SCREEN_COLUMNS);

  xpos = get_bios_cursor_x_position(s);
  ypos = get_bios_cursor_y_position(s);

  /* check for gfx mode */
  if(!using_text_mode()) gfx_mode = 1;

  switch (ch) {
  case '\r':         /* Carriage return */
    xpos = 0;
    break;

  case '\n':         /* Newline */
    ypos++;
    break;

  case 8:           /* Backspace */
    if (xpos > 0) xpos--;
    break;

  case '\t':        /* Tab */
    i10_deb("char_out: tab\n");
    do {
	char_out(' ', s);
	xpos = get_bios_cursor_x_position(s);
    } while (xpos % 8 != 0);
    break;

  case 7:           /* Bell */
    speaker_on(125, 0x637);
    return;

  default:          /* Printable character */
    if(gfx_mode) {
      vgaemu_put_char(ch, s, attr);
    }
    else
    {
      dst = screen_adr(s) + 2 * (ypos*co + xpos);
      vga_write(dst, ch);
      if(attr != -1) vga_write(dst + 1, attr);
    }
    xpos++;
  }

  if (xpos == co) {
    xpos = 0;
    ypos++;
  }
  if (ypos == li) {
    ypos--;
    if(gfx_mode)
      vgaemu_scroll(0, 0, co - 1, li - 1, 1, 0);
    else {
      /* Scroll with color newline */
      bios_scroll(0,0,co-1,li-1,1,
		  vga_read(screen_adr(s) + 2*(ypos*co + xpos) + 1));
    }
  }
  do_set_cursor_pos(s, xpos, ypos);
}

/* The following clears the screen buffer. It does it only to the screen
 * buffer.  If in termcap mode, the screen will be cleared the next time
 * restore_screen() is called.
 */
static void clear_screen(void)
{
  unsigned schar;
  u_short blank = ' ' | (7 << 8);
  int lx, s;

  if (config.dumb_video)
     return;

  v_printf("INT10: cleared screen: screen_adr %x\n", screen_adr(0));

  for (schar = screen_adr(0), lx = 0; lx < 16*1024;
       vga_write_word(schar, blank), lx++, schar+=2);

  for (s = 0; s < 8; s++)
    do_set_cursor_pos(s, 0, 0);
}

/* return number of vertical scanlines based on the bytes at
   40:88 and 40:89 */
static int get_text_scanlines(void)
{
  int info = READ_BYTE(BIOS_VIDEO_INFO_2);
  v_printf("scanlines=%x\n", info);
  if ((info & 0x80) && !(info & 0x10))
    return 200;
  if (!(info & 0x80) && !(info & 0x10))
    return 350;
  if (!(info & 0x80))
    return 400;
  return 480;
}

/* set number of vertical scanlines at the bytes at
   40:88 and 40:89 */
static void set_text_scanlines(int lines)
{
  int info = READ_BYTE(BIOS_VIDEO_INFO_2) & ~0x90;
  if (lines == 200)
    info |= 0x80;
  else {
    if (lines == 400)
      info |= 0x10;
    else if (lines == 480)
      info |= 0x90;
  }
  v_printf("scanlines=%x %d\n", info, lines);
  WRITE_BYTE(BIOS_VIDEO_INFO_2, info);
}

static int adjust_font_size(int vga_font_height)
{
  /* RBIL says:
     Recalculate: BIOS_FONT_HEIGHT, BIOS_ROWS_ON_SCREEN_MINUS_1, and
     BIOS_VIDEO_MEMORY_USED.
     Update CRTC registers 9 (for font height), A/B (cursor), 12 (display end),
     14 (underline location) */
  int li, text_scanlines;
  ioport_t port;

  if(vga_font_height == 0)
    return 0;

  i10_msg("adjust_font_size: font size %d lines\n", vga_font_height);

  text_scanlines = (READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1) *
    READ_WORD(BIOS_FONT_HEIGHT);
  if (text_scanlines <= 200) text_scanlines = 200;
  else if (text_scanlines <= 350) text_scanlines = 350;
  else if (text_scanlines <= 400) text_scanlines = 400;
  else text_scanlines = 480;
  li = text_scanlines / vga_font_height;
  if (li > MAX_LINES)
    return 0;
  text_scanlines = li * vga_font_height;

  port = READ_WORD(BIOS_VIDEO_PORT);
  port_outw(port, 0x12 | (((text_scanlines-1) & 0xff) << 8));
  port_outb(port, 0x14);
  port_outb(port + 1, (port_inb(port + 1) & ~0x1f) + vga_font_height);
  port_outb(port, 9);
  port_outb(port + 1, (port_inb(port + 1) & ~0x1f) + vga_font_height -1);
  WRITE_WORD(BIOS_FONT_HEIGHT, vga_font_height);

  if (using_mono_mode())
    set_cursor_shape(0x0b0d);
  else
    set_cursor_shape(0x0607);

  if (Video->setmode != NULL) {
    /* otherwise we can't change li */
    WRITE_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1, li - 1);
    WRITE_WORD(BIOS_VIDEO_MEMORY_USED,
	       TEXT_SIZE(READ_WORD(BIOS_SCREEN_COLUMNS), li));
  }
  return 1;
}

/*
 * set_video_mode() accepts both (S)VGA and VESA mode numbers
 * It should be fully compatible with the old set_video_mode()
 * function.
 * Note: bit 16 in "mode" is used internally to indicate that
 * nothing should be done except adjusting the font size.
 * -- 1998/04/04 sw
 */

bool set_video_mode(int mode)
{
  vga_mode_info *vmi;
  int clear_mem = 1;
  unsigned u;
  int co, li, vga_font_height, orig_mode;
  ioport_t port;

  if (config.dumb_video) {
    i10_msg("set_video_mode: no video!\n");
    return 0;
  }

  i10_msg("set_video_mode: mode 0x%02x\n", mode);

  if((vmi = vga_emu_find_mode(mode, NULL)) == NULL) {
    i10_msg("set_video_mode: undefined video mode\n");
    return 0;
  }
  if (vmi->mode_class == TEXT) {
    vga_mode_info *vmi2 = vmi;
    vga_mode_info *vmi_best = vmi;
    int ts = get_text_scanlines();
    int delta = ts - vmi2->height;

    i10_msg("look for mode with scan=%i font_height=%i\n",
        ts, READ_BYTE(BIOS_FONT_HEIGHT));
    while ((vmi2 = vga_emu_find_mode(mode, vmi2))) {
      if (vmi2->height > ts ||
          vmi2->char_height != READ_BYTE(BIOS_FONT_HEIGHT))
        continue;
      if (delta < 0 || ts - vmi2->height < delta) {
        i10_msg("better mode found: %ix%i %ix%i scan=%i\n",
            vmi2->text_width, vmi2->text_height,
            vmi2->char_width, vmi2->char_height,
            vmi2->height);
        vmi_best = vmi2;
        delta = ts - vmi2->height;
      }
      vmi = vmi_best;
    }
  }
  if (vmi->mode_class == GRAPH && config.term) {
    error("Cannot set graphics mode under terminal!\n");
    return 0;
  }
  if (!memcheck_is_reserved(vmi->buffer_start << 4, 0x8000, 'v')) {
    error("VGA: cannot set mode %i because of UMB at 0x%x\n",
	    mode, vmi->buffer_start);
    return 0;
  }

  orig_mode = mode;

  if(mode >= 0x80 && mode < 0x100) {
    mode &= 0x7f;
    clear_mem = 0;
  }
  if(mode & 0x8000) {
    mode &= ~0x8000;
    clear_mem = 0;
  }

  WRITE_BYTE(BIOS_CURRENT_SCREEN_PAGE, 0);
  WRITE_WORD(BIOS_VIDEO_MEMORY_ADDRESS, 0);
  WRITE_BYTE(BIOS_VIDEO_INFO_0, clear_mem ? 0x60 : 0xe0);
  MEMSET_DOS(0x450, 0, 0x10);	/* equiv. to set_bios_cursor_(x/y)_position(0..7, 0) */

  if(config.cardtype == CARD_MDA) mode = 7;

  if(vmi->type == TEXT_MONO) {
    WRITE_BYTE(BIOS_CONFIGURATION, READ_BYTE(BIOS_CONFIGURATION) | 0x30);
    port = 0x3b4;
  } else {
    WRITE_BYTE(BIOS_CONFIGURATION,
	       (READ_BYTE(BIOS_CONFIGURATION) & ~0x30) | 0x20);
    port = 0x3d4;
  }
  WRITE_WORD(BIOS_VIDEO_PORT, port);

  /*
   * We store the SVGA mode number (if possible) even when setting
   * a VESA mode.
   * Note that this gives mode 0x7f if no VGA mode number
   * had been assigned. -- sw
   */
  WRITE_BYTE(BIOS_VIDEO_MODE, vmi->VGA_mode & 0x7f);
  if (mode == 3)
    WRITE_BYTE(BIOS_VDU_CONTROL, 9);
  else if (vmi->type == TEXT_MONO)
    WRITE_BYTE(BIOS_VDU_CONTROL, 0xc);
  else if (vmi->mode_class == TEXT)
    WRITE_BYTE(BIOS_VDU_CONTROL, 8);
  else
    WRITE_BYTE(BIOS_VDU_CONTROL, 0xa);

  if (Video->setmode != NULL) {
    li = vmi->text_height;
    co = vmi->text_width;
  } else {
    li = READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1;
    co = READ_WORD(BIOS_SCREEN_COLUMNS);
  }

  video_mode = orig_mode;

#if USE_DUALMON
  /*
   * The following code (& comment) is taken literally
   * from the old set_video_mode(). Don't know what it's
   * for (or if it works). -- 1998/04/04 sw
   */

  /*
   * This to be sure in case of older DOS programs probing HGC.
   * There was no secure way to detect a HGC before VGA was invented.
   * ( Now we can do INT 10, AX=1A00 ).
   * Some older DOS programs do it by modifying EQUIP-flags
   * and then let the BIOS say, if it can ?!?!)
   * If we have config.dualmon, this happens legally.
   */
  if(vmi->type == TEXT_MONO && config.dualmon)
    vga_emu_setmode(7, co, li);
  else
#endif

  /* setmode needs video_mode to _still have_ the memory-clear bit -- sw */
  vga_emu_setmode(mode, co, li);

  /*
   * video_mode is expected to be the mode number _without_ the
   * memory-clear bit
   * -- sw
   */
  video_mode = mode;

  if(clear_mem && using_text_mode()) clear_screen();
  if (mode == 0x6)
    WRITE_BYTE(BIOS_VDU_COLOR_REGISTER, 0x3f);
  else if (mode <= 0x7 || vmi->type == TEXT_MONO)
    WRITE_BYTE(BIOS_VDU_COLOR_REGISTER, 0x30);

  vga_font_height = vmi->char_height;
  if (li <= MAX_LINES) {
    if(using_text_mode()) {
      port_outb(port, 9);
      port_outb(port + 1, (port_inb(port + 1) & ~0x1f) + vga_font_height -1);
      /* adjust number of scanlines in the CRT; setmode set it at 400 */
      if (vmi->height == 200) {
	/* just set doublescan */
	port_outb(port + 1, 0x80 | port_inb(port + 1));
      } else if (vmi->height != 400) {
	/* adjust display end */
	port_outw(port, 0x12 | (((vmi->height-1) & 0xff) << 8));
      }
      WRITE_WORD(BIOS_VIDEO_MEMORY_USED, TEXT_SIZE(co, li));
    } else {
      unsigned page_size = roundUpToNextPowerOfTwo(
	    (vga.scan_len * vga.height) | 0xfff);
      if (page_size > vga.mem.bank_pages * 4096)
        page_size = vga.mem.bank_pages * 4096;
      WRITE_WORD(BIOS_VIDEO_MEMORY_USED, page_size);
    }
    WRITE_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1, li - 1);
    WRITE_WORD(BIOS_SCREEN_COLUMNS, co);
  }

  switch(vga_font_height) {
    case 14:
      u = vgaemu_bios.font_14;
      break;
    case 16:
      u = vgaemu_bios.font_16;
      break;
    default:
      u = vgaemu_bios.font_8;
  }
  SETIVEC(0x43, 0xc000, u);
  WRITE_WORD(BIOS_FONT_HEIGHT, vga_font_height); // before set_cursor_shape()
  set_cursor_shape(vmi->type == TEXT_MONO ? 0x0b0d : 0x0607);

  if (using_text_mode()) {
    v_printf("INT10: X_set_video_mode: 8x%d ROM font -> bank 0\n",
             vga_font_height);
    vga_ROM_to_RAM(vga_font_height, 0); /* 0 is default bank */
    i10_msg("activated font bank 0\n");
  }

  return 1;
}

/* get the active and alternate display combination code */
static void get_dcc(int *active_dcc, int *alternate_dcc)
{
  int cur_video_combo = READ_BYTE(BIOS_VIDEO_COMBO);

#if USE_DUALMON
  if (config.dualmon) {
    if (IS_SCREENMODE_MDA) {
      *active_dcc = MDA_VIDEO_COMBO;  /* active display */
      *alternate_dcc = cur_video_combo;
    }
    else {
      *active_dcc = cur_video_combo;     /* active display */
      *alternate_dcc = MDA_VIDEO_COMBO;
    }
    return;
  }
#endif
  *active_dcc = cur_video_combo;	/* active display */
  *alternate_dcc = 0;		/* no inactive display */
}

/* INT 10 AH=1B - FUNCTIONALITY/STATE INFORMATION (PS,VGA/MCGA) */
static void return_state(unsigned int statebuf) {
	int active_dcc, alternate_dcc;

	WRITE_WORD(statebuf, vgaemu_bios.functionality - 0xc0000);
	WRITE_WORD(statebuf + 2, 0xc000);

	/* store bios 0:449-0:466 at ofs 0x04 */
	MEMCPY_DOS2DOS(statebuf + 0x04, 0x449, 0x466 - 0x449 + 1);
	/* store bios 0:484-0:486 at ofs 0x22 */
	MEMCPY_DOS2DOS(statebuf + 0x22, 0x484, 0x486 - 0x484 + 1);
	/* correct number of rows-1 to number of rows at offset 0x22 */
	WRITE_BYTE(statebuf + 0x22, READ_BYTE(statebuf + 0x22) + 1);
	get_dcc(&active_dcc, &alternate_dcc);
	WRITE_BYTE(statebuf + 0x25, active_dcc);
	WRITE_BYTE(statebuf + 0x26, alternate_dcc);
	WRITE_BYTE(statebuf + 0x27, 16); /* XXX number of colors, low byte */
	WRITE_BYTE(statebuf + 0x28, 0);  /* XXX number of colors, high byte */
	WRITE_BYTE(statebuf + 0x29, 8); /* XXX number of pages supported */
	WRITE_BYTE(statebuf + 0x2a, 2); /* XXX number of scanlines 0-3=200,350,400,480 */
	WRITE_BYTE(statebuf + 0x2b, 0); /* XXX primary character block */
	WRITE_BYTE(statebuf + 0x2c, 0); /* XXX secondary character block */
	WRITE_BYTE(statebuf + 0x31, 3); /* video memory: 3 = 256K */
	WRITE_BYTE(statebuf + 0x32, 0); /* XXX save pointer state flags */
	MEMSET_DOS(statebuf + 0x33, 0, 13);
}

/* helpers for font processing - eric@coli.uni-sb.de  11/2002 */
/* only for TEXT mode: Otherwise, int 0x43 is used...         */

static void vga_RAM_to_RAM(unsigned height, unsigned char chr, unsigned count,
                           unsigned seg, unsigned ofs, int bank)
{
  unsigned char *dst;
  unsigned i;
  unsigned long src;
  unsigned bankofs;
  if (!count) {
    v_printf("Tried to load 0 characters of font data???\n");
    return;
  }
  src = seg;
  src <<= 4;
  src += ofs;
  bankofs = ((bank & 3) << 1) + ((bank & 4) >> 2);
  bankofs <<= 13; /* unit is 8k */
  i10_msg("load 8x%d font (char %d..%d) 0x%04x:0x%04x -> bank %d\n",
          height, chr, chr+count-1, seg, ofs, bank);
  dst = vga.mem.base + 0x20000 + bankofs;
  /* copy count characters of height bytes each to vga_font_mem */
  for(i = chr; i < chr + count; i++) {
    MEMCPY_2UNIX(dst + i * 32, src + (i - chr) * height, height);
    if (height < 32)
      memset(dst + i * 32 + height, 0, 32 - height);
  }
  vga.reconfig.mem = 1;
}

static void vga_ROM_to_RAM(unsigned height, int bank)
{
  unsigned seg, ofs;
  seg = 0xc000;
  switch (height) { /* ALTERNATE ROM fonts not yet usable! */
  case 8:
    ofs = vgaemu_bios.font_8;
    break;
  case 14:
    ofs = vgaemu_bios.font_14;
    break;
  case 16:
    ofs = vgaemu_bios.font_16;
    break;
  default:
    v_printf("Error! Tried to load 8x%d ROM font!?\n",height);
    ofs = vgaemu_bios.font_16;
  }
  vga_RAM_to_RAM(height,0,256,seg,ofs,bank);
  memcpy(vga.backup_font, vga.mem.base + 0x20000, 256 * 32);
}

/******************************************************************/

/* the actual int10 handler */

int int10(void) /* with dualmon */
{
  /* some code here is copied from Alan Cox ***************/
  int x, y, co, li;
  unsigned page, page_size, address;
  unsigned sm;

#if 0 && USE_DUALMON
  static int last_equip=-1;

  if (config.dualmon && (last_equip != BIOS_CONFIG_SCREEN_MODE)) {
    v_printf("VID: int10 entry, equip-flags=0x%04x\n",READ_WORD(BIOS_CONFIGURATION));
    last_equip = BIOS_CONFIG_SCREEN_MODE;
    if (IS_SCREENMODE_MDA) Video->update_screen = NULL;
    else Video->update_screen = Video_default->update_screen;
  }
#endif

  li= READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1) + 1;
  co= READ_WORD(BIOS_SCREEN_COLUMNS);

  if (debug_level('v') >= 3)
    {
      if (debug_level('v') >= 4)
	i10_msg("near %04x:%08x\n", READ_SEG_REG(cs), REG(eip));
      if ( (LO(ax) >= ' ') && (LO(ax) < 0x7f) )
	i10_msg("AH=%02x AL=%02x '%c'\n",
		    HI(ax), LO(ax), LO(ax));
      else
	i10_msg("AH=%02x AL=%02x\n", HI(ax), LO(ax));
    }

#if 0
  i10_msg("ax %04x, bx %04x\n",LWORD(eax), LWORD(ebx));
#endif

  switch(HI(ax)) {
    case 0x00:		/* set video mode */
      i10_msg("set video mode: 0x%x\n", LO(ax));
      if(!set_video_mode(LO(ax))) {
        i10_msg("set_video_mode() failed\n");
      }
      break;


    case 0x01:		/* set text cursor shape */
      i10_deb("set text cursor shape: %u-%u\n", HI(cx), LO(cx));
      set_cursor_shape(LWORD(ecx));
      break;


    case 0x02:		/* set cursor pos */
      page = HI(bx);
      x = LO(dx);
      y = HI(dx);
      i10_deb("set cursor pos: page %d, x.y %d.%d\n", page, x, y);
      if(page > 7) {
        i10_msg("set cursor pos: page > 7: %d\n", page);
        return 1;
      }
      if (x >= co || y >= li) {
        /* some apps use this to hide the cursor,
         * we move it 1 char behind the visible part
         */
        x = co;
        y = li -1;
      }

      set_cursor_pos(page, x, y);
      break;


    case 0x03:		/* get cursor pos/shape */
      /* output start & end scanline even if the requested page is invalid */
      LWORD(ecx) = READ_WORD(BIOS_CURSOR_SHAPE);

      page = HI(bx);
      if (page > 7) {
        LWORD(edx) = 0;
        i10_msg("get cursor pos: page > 7: %d\n", page);
      } else {
        LO(dx) = get_bios_cursor_x_position(page);
        HI(dx) = get_bios_cursor_y_position(page);
      }

      i10_deb(
        "get cursor pos: page %u, x.y %u.%u, shape %u-%u\n",
        page, LO(dx), HI(dx), HI(cx), LO(cx)
      );
      break;


    case 0x04:		/* read light pen pos */
      i10_msg("read light pen pos: NOT IMPLEMENTED\n");
      HI(ax) = 0;	/* "light pen switch not pressed" */
      			/* This is how my VGA BIOS behaves [rz] */
      break;


    case 0x05:		/* set active display page */
#if USE_DUALMON
      if (config.dualmon && IS_SCREENMODE_MDA) break;
#endif
      page = LO(ax);
      i10_deb("set display page: from %d to %d\n", READ_BYTE(BIOS_CURRENT_SCREEN_PAGE), page);
      if(page > max_page) {
	i10_msg("set display page: bad page %d\n", page);
	break;
      }
      page_size = READ_WORD(BIOS_VIDEO_MEMORY_USED);
      address = page_size * page;
      crt_outw(0xc, address/(using_text_mode() ? 2 : 1));

      WRITE_BYTE(BIOS_CURRENT_SCREEN_PAGE, page);
      WRITE_WORD(BIOS_VIDEO_MEMORY_ADDRESS, address);
      x = get_bios_cursor_x_position(page);
      y = get_bios_cursor_y_position(page);
      set_cursor_pos(page, x, y);
      break;


    case 0x06:		/* scroll up */
      reset_idle(0);
      i10_deb(
        "scroll up: %u lines, area %u.%u-%u.%u, attr 0x%02x\n",
        LO(ax), LO(cx), HI(cx), LO(dx), HI(dx), HI(bx)
      );
      if(using_text_mode()) {
        bios_scroll(LO(cx), HI(cx), LO(dx), HI(dx), LO(ax), HI(bx));
      }
      else {
        vgaemu_scroll(LO(cx), HI(cx), LO(dx), HI(dx), LO(ax), HI(bx));
      }
      break;


    case 0x07:		/* scroll down */
      reset_idle(0);
      i10_deb(
        "scroll dn: %u lines, area %u.%u-%u.%u, attr 0x%02x\n",
        LO(ax), LO(cx), HI(cx), LO(dx), HI(dx), HI(bx)
      );
      if(using_text_mode()) {
        bios_scroll(LO(cx), HI(cx), LO(dx), HI(dx), -LO(ax), HI(bx));
      }
      else {
        vgaemu_scroll(LO(cx), HI(cx), LO(dx), HI(dx), -LO(ax), HI(bx));
      }
      break;


    case 0x08:		/* read character & attr at x,y */
      page = HI(bx);
      if (page > max_page) {
        i10_msg("read char: invalid page %d\n", page);
        break;
      }
      if(using_text_mode()) {
        sm = screen_adr(page);
        LWORD(eax) = vga_read_word(sm + (co * get_bios_cursor_y_position(page)
				   + get_bios_cursor_x_position(page)) * 2);
      }
      else {
        LWORD(eax) = 0;
      }
      i10_deb(
        "read char: char(%d.%d) = 0x%02x, attr 0x%02x\n",
        get_bios_cursor_x_position(page), get_bios_cursor_y_position(page),
        LO(ax), HI(ax)
      );
      break;


    /* these two put literal character codes into memory, and do
       * not scroll or move the cursor...
       * the difference is that 0xA ignores color for text modes
       */
    case 0x09:		/* write char & attr */
      reset_idle(0);
      i10_deb(
          "rep char: page %u, char 0x%02x '%c', attr 0x%02x\n",
          HI(bx), LO(ax), LO(ax) > ' ' && LO(ax) < 0x7f ? LO(ax) : ' ', LO(bx)
      );
      if (config.dumb_video) {
        FILE *f = config.tty_stderr ? stderr : stdout;
        int i;

        if (no_local_video && !config.tty_stderr)
          break;
        for (i = 0; i < LWORD(ecx); i++)
          fputc(LO(ax), f);
        /* cursor must not move when printing */
        fputc('\r', f);
      } else {
        vgaemu_repeat_char_attr(LO(ax), HI(bx), LO(bx), LWORD(ecx));
      }
      break;

    case 0x0a:		/* write char */
      reset_idle(0);
      i10_deb(
          "rep char: page %u, char 0x%02x '%c'\n",
          HI(bx), LO(ax), LO(ax) > ' ' && LO(ax) < 0x7f ? LO(ax) : ' '
      );
      vgaemu_repeat_char(LO(ax), HI(bx), LO(bx), LWORD(ecx));
      break;


    case 0x0b:		/* set palette/bg/border color */
      {
	unsigned char currentpalette = READ_BYTE(BIOS_VDU_COLOR_REGISTER);
	i10_msg("set palette or bg/border, bx=%x\n",LWORD(ebx));
	if (HI(bx) == 0) {
	  currentpalette &= ~0x1f;
	  currentpalette |= LO(bx) & 0x1f;
	} else if (HI(bx) == 1) {
	  if (LO(bx))
	    currentpalette |= 0x20;
	  else
	    currentpalette &= ~0x20;
	} else {
	  break;
	}
	Misc_set_color_select(currentpalette);
	WRITE_BYTE(BIOS_VDU_COLOR_REGISTER, currentpalette);
      }
      break;

    case 0x0c:		/* write pixel */
      reset_idle(0);
      if(!using_text_mode())
        vgaemu_put_pixel(LWORD(ecx), LWORD(edx), HI(bx), LO(ax));
      break;


    case 0x0d:		/* read pixel */
      LO(ax) = 0;
      if(!using_text_mode()) {
        LO(ax) = vgaemu_get_pixel(LWORD(ecx), LWORD(edx), HI(bx));
        i10_msg("read pixel: 0x%02x\n", LO(ax));
      }
      break;

    case 0x0e:		/* print char */
      reset_idle(0);
      if(using_text_mode()) {
        i10_deb(
          "tty put char: page %u, char 0x%02x '%c'\n",
          HI(bx), LO(ax), LO(ax) > ' ' && LO(ax) < 0x7f ? LO(ax) : ' '
        );
        char_out(LO(ax), READ_BYTE(BIOS_CURRENT_SCREEN_PAGE));
      }
      else {
        i10_deb(
          "tty put char: page %u, char 0x%02x '%c', attr 0x%02x\n",
          HI(bx), LO(ax), LO(ax) > ' ' && LO(ax) < 0x7f ? LO(ax) : ' ', LO(bx)
        );
        tty_char_out(LO(ax), READ_BYTE(BIOS_CURRENT_SCREEN_PAGE), LO(bx));
      }
      break;

    case 0x0f:		/* get video mode */
/*        EGA, VGA, and UltraVision return either AL=03h (color) or AL=07h
          (monochrome) in all extended-row text modes */
      if (using_text_mode() && li > 25)
        LO(ax) = (using_mono_mode() ? 7 : 3);
      else
        LO(ax) = READ_BYTE(BIOS_VIDEO_MODE) |
	    (READ_BYTE(BIOS_VIDEO_INFO_0) & 0x80);
      HI(ax) = co & 0xff;
      HI(bx) = READ_BYTE(BIOS_CURRENT_SCREEN_PAGE);
      i10_deb(
        "get video mode: mode 0x%02x, page %u, %u columns\n",
        LO(ax), HI(bx), HI(ax)
      );
      break;


    case 0x10:		/* ega/vga palette functions */
      i10_deb("ega/vga palette: sub function 0x%02x\n", LO(ax));

      /* root@zaphod */
      /* Palette register stuff. Only for the VGA emulator */
      {
         int i, count;
         unsigned int src;
         unsigned char index, m;
         DAC_entry rgb;

         switch(LO(ax)) {
           case 0x00:	/* Set Palette Register */
             Attr_set_entry(LO(bx), HI(bx));
             break;

           case 0x01:	/* Set Overscan Color */
             Attr_set_entry(0x11, HI(bx));
             break;

           case 0x02:	/* Set Palette & Overscan Color */
             src = SEGOFF2LINEAR(SREG(es), LWORD(edx));
             for(i = 0; i < 0x10; i++) Attr_set_entry(i, READ_BYTE(src + i));
             Attr_set_entry(0x11, READ_BYTE(src + i));
             break;

           case 0x03:	/* Toggle Intensity/Blinking Bit */
             m = Attr_get_entry(0x10) & ~(1 << 3);
             m |= (LO(bx) & 1) << 3;
             Attr_set_entry(0x10, m);
             break;

           case 0x07:	/* Read Palette Register */
             HI(bx) = Attr_get_entry(LO(bx));
             break;

           case 0x08:	/* Read Overscan Color */
             HI(bx) = Attr_get_entry(0x11);
             break;

           case 0x09:	/* Read Palette & Overscan Color */
             src = SEGOFF2LINEAR(SREG(es), LWORD(edx));
             for(i = 0; i < 0x10; i++) WRITE_BYTE(src + i, Attr_get_entry(i));
             WRITE_BYTE(src + i, Attr_get_entry(0x11));
             break;

           case 0x10:	/* Set Individual DAC Register */
             DAC_set_entry(LO(bx), HI(dx), HI(cx), LO(cx));
             break;

           case 0x12:	/* Set Block of DAC Registers */
             index = LO(bx);
             count = LWORD(ecx);
             src = SEGOFF2LINEAR(SREG(es), LWORD(edx));
             for(i = 0; i < count; i++, index++)
               DAC_set_entry(index, READ_BYTE(src + 3*i),
			    READ_BYTE(src + 3*i + 1), READ_BYTE(src + 3*i + 2));
             break;

           case 0x13:	/* Select Video DAC Color Page */
             m = Attr_get_entry(0x10);
             switch(LO(bx)) {
               case 0:	/* Select Page Mode */
                 m &= ~(1 << 7);
                 m |= (HI(bx) & 1) << 7;
                 Attr_set_entry(0x10, m);
                 break;
               case 1:	/* Select Page */
                 if(m & (1 << 7))
                   Attr_set_entry(0x14, HI(bx) & 0xf);
                 else
                   Attr_set_entry(0x14, (HI(bx) & 0x3) << 2);
                 break;
             }
             break;

           case 0x15:	/* Read Individual DAC Register */
             DAC_get_entry(&rgb, LO(bx));
             HI(dx) = rgb.r; HI(cx) = rgb.g; LO(cx) = rgb.b;
             break;

           case 0x17:	/* Read Block of DAC Registers */
             index = LO(bx);
             count = LWORD(ecx);
             src = SEGOFF2LINEAR(SREG(es), LWORD(edx));
             for(i = 0; i < count; i++, index++) {
               DAC_get_entry(&rgb, index);
               WRITE_BYTE(src + 3*i, rgb.r);
               WRITE_BYTE(src + 3*i + 1, rgb.g);
               WRITE_BYTE(src + 3*i + 2, rgb.b);
             }
             break;

           case 0x18:	/* Set PEL Mask */
             DAC_set_pel_mask(LO(bx));
             break;

           case 0x19:	/* Read PEL Mask */
             LO(bx) = DAC_get_pel_mask();
             break;

           case 0x1a:	/* Get Video DAC Color Page */
             LO(bx) = m = (Attr_get_entry(0x10) >> 7) & 1;
             HI(bx) = (Attr_get_entry(0x14) & 0xf) >> (m ? 0 : 2);
             break;

           case 0x1b:	/* Convert to Gray */
             for(index = LO(bx), count = LWORD(ecx); count--; index++)
               DAC_rgb2gray(index);
             break;

           default:
             i10_msg("ega/vga palette: invalid sub function 0x%02x\n", LO(ax));
             break;
         }
      }
      break;


    case 0x11:		/* character generator functions */
      {
	int vga_font_height;
        unsigned ofs, seg;
        unsigned rows, char_height;

        i10_msg("char gen: func 0x%02x, bx 0x%04x\n", LO(ax), LWORD(ebx));

        switch(LO(ax)) {
          case 0x03:
            /* For EGA: Select fonts xx and yy for attrib bit 3 1/0 */
            /* For VGA: Same, but using Xxx and Yyy. BL is 00XYxxyy */
            /* ***          see also vgaemu_put_char()         ***  */
            /* the X/Y bits mean 8k offset, xx/yy use 16k units     */
            /* see: sequencer, map select, data[3] ... sequemu.c    */
            Seq_set_index(3);
            Seq_write_value(LO(bx));
            i10_msg("sequencer char map select: 0x%02x\n", LO(bx));
            break;

          case 0x01:		/* load 8x14 charset */
          case 0x11:
            vga_font_height = 14;
            vga_ROM_to_RAM(14, LO(bx));
            goto more_lines;

          case 0x02:		/* load 8x8 charset */
          case 0x12:
            vga_font_height = 8;
            vga_ROM_to_RAM(8, LO(bx));
            goto more_lines;

          case 0x04:		/* load 8x16 charset */
          case 0x14:
            vga_font_height = 16;
            vga_ROM_to_RAM(16, LO(bx));
            goto more_lines;

          /* load a custom font */
          case 0x00:
          case 0x10:
            vga_font_height = HI(bx);
            /* *************************************************************
             * func 00 would not change as much as func 0x10, which would  *
             * reprogram registers 9 = bh-1 (mode 7 only, max scan line),  *
             * a = bh-2 (cursor start), b = 0 (cursor end),                *
             * 12 = (rows+1) / (bh-1) (vertical display end),              *
             * 14 = bh-1 (underline loc). Recalcluates CRT buffer length.  *
             * Max character rows is also recalculated, as well as by/char *
             * ... and page 0 must be active                               *
             * ES:BP -> table CX = count of chars DX = from which char on  *
             * BL = which block to load into map2 BH = bytes / char        *
             ************************************************************* */
            vga_RAM_to_RAM(HI(bx), LO(dx), LWORD(ecx),
                SREG(es), LWORD(ebp), LO(bx));
            i10_msg("some user font data loaded\n");
            goto more_lines;

          more_lines:
            /* Also activating the target bank - some programs */
            /* seem to assume this. Sigh...                    */

            Seq_set_index(3);     /* sequencer: character map select */
            /* available: x, y, c...!?    transforming bitfields...  */
            x = (LO(bx) & 3) | ((LO(bx) & 4) << 2);
            x |= ((LO(bx) & 3) << 2) | ((LO(bx) & 4) << 3);
            Seq_write_value(x);         /* set bank N for both fonts */
            i10_msg("activated font bank %d (0x%02x)\n",
                (LO(bx) & 7), x);
	    if(LO(ax) >= 0x10 && !adjust_font_size(vga_font_height))
	      v_printf("Problem changing font height %d->%d\n",
		       READ_WORD(BIOS_FONT_HEIGHT), vga_font_height);
	    break;

          case 0x20:		/* set 8x8 gfx chars */
            SETIVEC(0x1f, SREG(es), LWORD(ebp));
            i10_deb("set 8x8 gfx chars: addr 0x%04x:0x%04x\n", ISEG(0x1f), IOFF(0x1f));
            break;

          case 0x21:		/* load user gfx chars */
          case 0x22:		/* load 14x8 gfx chars */
          case 0x23:		/* load 8x8 gfx chars */
          case 0x24:		/* load 16x8 gfx chars */
          /* BL=0/1/2/3 for DL/14/25/43 rows                   */
          /* CX is byte / char, ES:BP is pointer for case 0x21 */
            seg = 0xc000;
            switch(LO(ax)) {
              case 0x21:
                ofs = LWORD(ebp);
                seg = SREG(es);
                char_height = LWORD(ecx);
                break;
              case 0x22:
                ofs = vgaemu_bios.font_14;
                char_height = 14;
                break;
              case 0x23:
                ofs = vgaemu_bios.font_8;
                char_height = 8;
                break;
              default:		/* case 0x24 */
                ofs = vgaemu_bios.font_16;
                char_height = 16;
                break;
            }
            rows = LO(dx); /* gets changed for BL != 0 now: */
            switch(LO(bx)) {
              case 1:
                rows = 14;
                break;
              case 2:
                rows = 25;
                break;
              case 3:
                rows = 43;
                break;
            }
            SETIVEC(0x43, seg, ofs);
            WRITE_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1, rows - 1);
            WRITE_WORD(BIOS_FONT_HEIGHT, char_height);
            /* Does NOT load any video font RAM                  */
            /* This is for GRAPHICS mode only. No vga_R*...      */
            /* No setting of the CRTC font size either...        */
            i10_msg(
              "load gfx font: height %u, rows %u, addr 0x%04x:0x%04x\n",
              char_height, rows, seg, ofs
            );
            break;

          case 0x30:		/* get current character generator info */
            LWORD(ecx) = READ_WORD(BIOS_FONT_HEIGHT);
            LO(dx) = READ_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1);
            seg = 0xc000;
            switch((HI(bx))) {
              case 0:
                ofs = IOFF(0x1f);
                seg = ISEG(0x1f);
                break;
              case 1:
                ofs = IOFF(0x43);
                seg = ISEG(0x43);
                break;
              case 2:
                ofs = vgaemu_bios.font_14;
                break;
              case 3:
                ofs = vgaemu_bios.font_8;
                break;
              case 4:
                ofs = vgaemu_bios.font_8 + 128 * 8;
                break;
              case 5:
                ofs = vgaemu_bios.font_14_alt;
                break;
              case 6:
                ofs = vgaemu_bios.font_16;
                break;
              case 7:
                ofs = vgaemu_bios.font_16_alt;
                break;
              default:
                seg = ofs = 0;
            }
            LWORD(ebp) = ofs;
            SREG(es) = seg;
            i10_deb(
              "font info: char height: %u, rows %u, font 0x%x, addr 0x%04x:0x%04x\n",
              LWORD(ecx), LO(dx) + 1, HI(bx), SREG(es), LWORD(ebp)
            );
            break;

          default:
            i10_msg("char gen: func 0x%02x NOT IMPLEMENTED\n", LO(ax));
        }
      }
      break;


    case 0x12:		/* alternate function select */
      switch(LO(bx)) {
        case 0x10:	/* get ega info */
          HI(bx) = READ_WORD(BIOS_VIDEO_PORT) == 0x3b4;
          LO(bx) = 3;
          HI(cx)=0xf;	/* feature bits (no feature controller) */
          LO(cx)=READ_BYTE(BIOS_VIDEO_INFO_1) & 0xf;
          i10_deb("get ega info: bx 0x%04x, cx 0x%04x\n", LWORD(ebx), LWORD(ecx));
          break;

        case 0x20:	/* select alternate print screen */
          /* Would install a handler of the video               */
          /* BIOS that can do more modes than the main BIOS     */
          /* handler (which is often limited to 80x25 text)     */
          i10_deb("select alternate prtsc: NOT IMPLEMENTED\n");
          break;

        case 0x30:	/* select vertical resolution */
          {
            static int scanlines[4] = {200, 350, 400, 480};
            /* 480 (undocumented) (int 0x10 AH=12 BL=30)          */
            if((unsigned) LO(ax) < 4) {
	      int text_scanlines = scanlines[LO(ax)];
	      set_text_scanlines(text_scanlines);
              LO(ax) = 0x12;
              i10_deb("select vert res: %d lines", text_scanlines);
            }
            else {
              i10_msg("select vert res: invalid arg 0x%02x", LO(ax));
            }
          }
          break;

        case 0x32:	/* enable/disable cpu access to video ram */
          /* FIXME: implement this XXX */
          /* would probably modify port 0x3c2, misc output, which */
          /* also is responsible for 3b4<->3d4, pixelclock, sync  */
          /* polarity, odd/even page selection, ...               */
          if(LO(ax) == 0)
            i10_deb("disable cpu access to video (ignored)\n");
          else
            i10_deb("enable cpu access to video (ignored)\n");
          break;

#if 0
        case 0x34:	/* enable/disable cursor emulation */
          cursor_emulation = LO(ax);
          LO(ax) = 0x12;
          i10_deb("cursor emulation: %s\n", cursor_emulation & 1 ? "disabled" : "enabled");
          break;
#endif

        case 0x36:	/* video screen on/off */
          /* VGAEMU oriented (do port 3c0 flag as well?)            */
          /* (probably not: port 0x3c0|=0x20 -> all overscan color) */
          /* FIXME: only useful when using VGAEMU, add a #define    */
          /* Will influence vga.config.video_off -> X_update_screen */
          Seq_set_index(1);             /* sequencer: clocking mode */
          if(LO(ax) == 0) {
            i10_deb("turn video screen on (partially ignored)\n");
             Seq_write_value(vga.seq.data[1] & ~0x20);
             /* bit 0x20 is screen refresh off */
          } else {
             i10_deb("turn video screen off (partially ignored)\n");
             Seq_write_value(vga.seq.data[1] | 0x20);
             /* bit 0x20 is screen refresh off */
          }
#if 0
          LO(ax) = 0x12;
#endif
          break;

        default:
          i10_msg("video subsys config: function 0x%02x NOT IMPLEMENTED\n", LO(bx));
      }
      break;


    case 0x13:		/* write string */
      {
        int with_attr = (LO(ax) & 2) >> 1;
        unsigned page = HI(bx);
        unsigned char attr = LO(bx);
        unsigned len = LWORD(ecx);
        unsigned int str = SEGOFF2LINEAR(SREG(es), LWORD(ebp));
        unsigned old_x, old_y;

        reset_idle(0);
        old_x = get_bios_cursor_x_position(page);
        old_y = get_bios_cursor_y_position(page);

	set_cursor_pos(page, LO(dx), HI(dx));

        i10_deb(
          "write string: page %u, x.y %d.%d, attr 0x%02x, len %u, addr 0x%04x:0x%04x\n",
          page, LO(dx), HI(dx), attr, len, SREG(es), LWORD(ebp)
        );
#if DEBUG_INT10
        i10_deb("write string: str \"");
        {
          unsigned u;

          for(u = 0; u < len; u++)
            v_printf("%c", READ_BYTE(str+u) >= ' ' &&
		     READ_BYTE(str+u) < 0x7f ? READ_BYTE(str+u) : ' ');
          v_printf("\"\n");
        }
#endif

        if(with_attr) {
          while(len--) {
            tty_char_out(READ_BYTE(str), page, READ_BYTE(str+1));
            str += 2;
          }
        }
        else {
          while(len--) tty_char_out(READ_BYTE(str++), page, attr);
        }

        if(!(LO(ax) & 1)) {	/* no cursor update */
	  set_cursor_pos(page, old_x, old_y);
        }
      }
      break;

    case 0x1a:		/* get/set display combo */
      if(LO(ax) == 0) {
        int active_dcc, alternate_dcc;

        LO(ax) = 0x1a;		/* valid function=0x1a */
        get_dcc(&active_dcc, &alternate_dcc);
        LO(bx) = active_dcc;
        HI(bx) = alternate_dcc;
        i10_deb("get display combo: active 0x%02x, alternate 0x%2x\n", LO(bx), HI(bx));
      }
      else {
        i10_msg("set display combo: NOT IMPLEMENTED\n");
      }
      break;


    case 0x1b:		/* functionality/state information */
      if(LWORD(ebx) == 0) {
        i10_deb("get functionality/state info\n");
        return_state(SEGOFF2LINEAR(SREG(es), LWORD(edi)));
        LO(ax) = 0x1b;
      } else {
        i10_msg("unknown functionality/state request: 0x%04x", LWORD(ebx));
      }
      break;


    case 0x1c:	{	/* save/restore video state */
      unsigned base = _BX;
      if (LO(ax) > 2)
	break;
      switch(LO(ax)) {
      case 0: {
	unsigned size = 0;
	i10_msg("save/restore: return state buffer size, cl=%x\n", LO(cx));
	if (LO(cx) & 1) {
	  /* video hardware */
	  size += 0x46;
	}
	if (LO(cx) & 2) {
	  /* BIOS */
	  size += 96;
	}
	if (LO(cx) & 4) {
	  /* DAC */
	  size += 0x304;
	}
	LWORD(ebx) = (size + 63)/64;
	break;
      }
      case 1:
	i10_msg("save/restore: save state, cl=%x\n", LO(cx));
	if (LO(cx) & 1) {
	  unsigned char buf[0x46];
	  unsigned crtc, ind;

	  /* select crtc base address */
	  crtc = (port_inb(MISC_OUTPUT_R) & 1) ? 0x3d4 : 0x3b4;

	  buf[0x0] = port_inb(SEQUENCER_INDEX);
	  buf[0x1] = port_inb(crtc);
	  buf[0x2] = port_inb(GFX_INDEX);
	  /* feature control */
	  buf[0x4] = port_inb(FEATURE_CONTROL_R);

	  for (ind = 1; ind < 5; ind++) {
	    port_outb(SEQUENCER_INDEX, ind);
	    buf[0x4+ind] = port_inb(SEQUENCER_DATA);
	  }
	  port_outb(SEQUENCER_INDEX, 0);
	  buf[0x9] = port_inb(SEQUENCER_DATA);

	  for (ind = 0; ind < 25; ind++) {
	    port_outb(crtc, ind);
	    buf[0x0a+ind] = port_inb(crtc + 1);
	  }

	  /* reset flipflop ! */
	  port_inb(crtc + 0x6);
	  buf[0x3] = port_inb(ATTRIBUTE_INDEX);
	  for (ind = 0; ind < 20; ind++) {
	    port_inb(crtc + 0x6);
	    port_outb(ATTRIBUTE_INDEX, ind);
	    buf[0x23 + ind] = port_inb(ATTRIBUTE_DATA);
	  }
	  port_inb(crtc + 0x6);
	  port_outb(ATTRIBUTE_INDEX, buf[0x3]);
	  port_inb(crtc + 0x6);

	  for (ind = 0; ind < 9; ind++) {
	    port_outb(GFX_INDEX, ind);
	    buf[0x37+ind] = port_inb(GFX_DATA);
	  }

	  buf[0x40] = crtc & 0xff;
	  buf[0x41] = crtc >> 8;
	  /* VGA latches */
	  memcpy(&buf[0x42], vga.latch, 4);
	  MEMCPY_2DOS(SEGOFF2LINEAR(_ES, base), buf, sizeof(buf));
	  base += sizeof(buf);
	}
	if (LO(cx) & 2) {
	  MEMCPY_DOS2DOS(SEGOFF2LINEAR(_ES, base), 0x449, 96);
	  base += 96;
	}
	if (LO(cx) & 4) {
	  unsigned char buf[0x304];
	  unsigned ind;
	  buf[0] = port_inb(DAC_STATE);
	  buf[1] = port_inb(DAC_WRITE_INDEX);
	  buf[2] = port_inb(DAC_PEL_MASK);
	  port_outb(DAC_READ_INDEX, 0x00);
	  for(ind = 0; ind < 768; ind++)
	    buf[0x3 + ind] = port_inb(DAC_DATA);
	  buf[0x303] = port_inb(COLOR_SELECT);
	  MEMCPY_2DOS(SEGOFF2LINEAR(SREG(es), base), buf, sizeof(buf));
	  base += sizeof(buf);
	}
	break;
      case 2:
	i10_msg("save/restore: restore state, cl=%x\n", LO(cx));
	if (LO(cx) & 1) {
	  unsigned char buf[0x46];
	  unsigned crtc, ind;
	  MEMCPY_2UNIX(buf, SEGOFF2LINEAR(SREG(es), base), sizeof(buf));
	  base += sizeof(buf);
	  crtc = buf[0x40] | (buf[0x41] << 8);
	  for (ind = 1; ind < 5; ind++)
	    port_outw(SEQUENCER_INDEX, ind | (buf[0x04+ind] << 8));
	  port_outw(SEQUENCER_INDEX, buf[0x09] << 8);
	  /* disable write protection to index 0-7 */
	  port_outw(crtc, 0x0011);
	  for (ind = 0; ind < 25; ind++)
	    port_outw(crtc, ind | (buf[0x0a+ind] << 8));
	  /* select crtc base address */
	  port_outb(MISC_OUTPUT_W, (port_inb(MISC_OUTPUT_R) & ~0x01) | (crtc == 0x3d4));
	  /* reset flipflop ! */
	  port_inb(crtc + 0x6);
	  for (ind = 0; ind < 20; ind++) {
	    port_outb(ATTRIBUTE_INDEX, ind);
	    port_outb(ATTRIBUTE_INDEX, buf[0x23+ind]);
	  }
	  port_outb(ATTRIBUTE_INDEX, buf[0x3]);
	  port_inb(crtc + 0x6);
	  for (ind = 0; ind < 9; ind++)
	    port_outw(GFX_INDEX, ind | (buf[0x37+ind] << 8));

	  port_outb(SEQUENCER_INDEX, buf[0x0]);
	  port_outb(crtc, buf[0x1]);
	  port_outb(GFX_INDEX, buf[0x2]);
	  /* feature control */
	  port_outb(crtc + 0x6, buf[0x4]);
	  /* VGA latches */
	  memcpy(vga.latch, &buf[0x42], 4);
	}
	if (LO(cx) & 2) {
	  MEMCPY_DOS2DOS(0x449, SEGOFF2LINEAR(_ES, base), 96);
	  base += 96;
	}
	if (LO(cx) & 4) {
	  unsigned char buf[0x304];
	  unsigned ind;
	  MEMCPY_2UNIX(buf, SEGOFF2LINEAR(_ES, base), sizeof(buf));
	  base += sizeof(buf);
	  port_outb(DAC_PEL_MASK, buf[2]);
	  port_outb(DAC_WRITE_INDEX, 0x00);
	  for(ind = 0; ind < 768; ind++)
	    port_outb(DAC_DATA, buf[0x3 + ind]);
	  port_outb(COLOR_SELECT, buf[0x303]);
	  if (buf[0] & 3)
	    port_outb(DAC_READ_INDEX, buf[1]);
	  else
	    port_outb(DAC_WRITE_INDEX, buf[1]);
	}
	break;
      }
      LO(ax) = 0x1c;
      break;
    }


    case 0x4f:		/* vesa interrupt */
      do_vesa_int();
      break;


    case 0xcc:		/* called from NC 5.0 */
      _CX = 0; _AL = 0xff;
      i10_deb("obscure function 0x%02x\n", HI(ax));
      break;


    case 0xfe:		/* get shadow buffer..return unchanged */
    case 0xff:		/* update shadow buffer...do nothing */
      i10_deb("obscure function 0x%02x\n", HI(ax));
      break;


    default:
      i10_msg("unknown video int 0x%04x\n", LWORD(eax));
      break;
  }
  return 1;
}

void video_mem_setup(void)
{
  int co, li;

  WRITE_BYTE(BIOS_CURRENT_SCREEN_PAGE, 0);

  li = LI;
  co = CO;
  if (config.term)
    gettermcap(0, &co, &li);

  WRITE_WORD(BIOS_SCREEN_COLUMNS, co);     /* chars per line */
  WRITE_BYTE(BIOS_ROWS_ON_SCREEN_MINUS_1, li - 1); /* lines on screen - 1 */
  WRITE_WORD(BIOS_VIDEO_MEMORY_USED, TEXT_SIZE(co,li));   /* size of video regen area in bytes */

  WRITE_WORD(BIOS_CURSOR_SHAPE, (bios_configuration&MDA_CONF_SCREEN_MODE)?0x0A0B:0x0607);
#if 0
  /* This is needed in the video stuff. Grabbed from boot(). */
  if ((bios_configuration & MDA_CONF_SCREEN_MODE) == MDA_CONF_SCREEN_MODE) {
    WRITE_WORD(BIOS_VIDEO_PORT, 0x3b4);	/* base port of CRTC - IMPORTANT! */
    video_mode = 7;
  } else {
    WRITE_WORD(BIOS_VIDEO_PORT, 0x3d4);	/* base port of CRTC - IMPORTANT! */
    video_mode = 3;
  }
  WRITE_BYTE(BIOS_VIDEO_MODE, video_mode);

  WRITE_BYTE(BIOS_VDU_CONTROL, 9);	/* current 3x8 (x=b or d) value */

  WRITE_WORD(BIOS_VIDEO_MEMORY_ADDRESS, 0);/* offset of current page in buffer */

  WRITE_WORD(BIOS_FONT_HEIGHT, 16);
#endif
  /* XXX - these are the values for VGA color!
     should reflect the real display hardware. */
  WRITE_BYTE(BIOS_VIDEO_INFO_0, 0x60);
  WRITE_BYTE(BIOS_VIDEO_INFO_1, 0xF9);
  WRITE_BYTE(BIOS_VIDEO_INFO_2, 0x51);

  /* XXX - This usage isn't quite correct: this byte should be an index
     into a table. Requires modification of our BIOS, and setting up
     lots of tables*/
  WRITE_BYTE(BIOS_VIDEO_COMBO, video_combo);

  if (!config.vga) {
    WRITE_DWORD(BIOS_VIDEO_SAVEPTR, 0);		/* pointer to video table */
    /* point int 1f to the default 8x8 graphics font for high characters */
    SETIVEC(0x1f, 0xc000, vgaemu_bios.font_8 + 128 * 8);
  }
  else if (!config.vbios_post) {
    Bit32u p, q;
    Bit16u vc;

    i10_msg("Now initialising 0x40:a8-ab\n");
    WRITE_DWORD(BIOS_VIDEO_SAVEPTR, int_bios_area[BIOS_VIDEO_SAVEPTR/4]);

    /* many BIOSes use this: take as fallback value */
    WRITE_BYTE(BIOS_VIDEO_COMBO, 0xb);
    /* correct BIOS_VIDEO_COMBO value */
    p = READ_DWORD(BIOS_VIDEO_SAVEPTR) + 0x10;
    /* [VGA only] ptr to Secondary Save Pointer Table, must be valid */
    p = rFAR_PTR(Bit32u, p);
    p = READ_DWORD(p) + 0x2;
    /* ptr to Display Combination Code Table, must be valid */
    p = rFAR_PTR(Bit32u, p);
    p = READ_DWORD(p) + 0x4;
    /* Each pair of bytes gives a valid display combination */
    q = p = rFAR_PTR(Bit32u, p);
    do {
      vc = READ_WORD(q);
      if (vc == video_combo || vc == (video_combo << 8)) {
	WRITE_BYTE(BIOS_VIDEO_COMBO, (q-p)/2);
	i10_msg("found video_combo: %x\n", (q-p)/2);
	break;
      }
      q += 2;
    } while ((vc & 0xff) < 0xd && vc < 0xd00);
  }
}

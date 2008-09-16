#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <termios.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "term.h"
#include "termbox.h"

struct cellbuf {
	unsigned int width;
	unsigned int height;
	struct tb_cell *cells;
};

#define CELL(buf, x, y) (buf)->cells[(y) * (buf)->width + (x)]

static struct termios orig_tios;

static struct cellbuf back_buffer;
static struct cellbuf front_buffer;
static unsigned int termw;
static unsigned int termh;

static int inputmode = TB_INPUT_ESC;

static struct ringbuffer inbuf;

static FILE *out;
static FILE *in;

static int out_fileno;
static int in_fileno;

static volatile int sigwinch_r;

static void cellbuf_init(struct cellbuf *buf, unsigned int width, unsigned int height);
static void cellbuf_resize(struct cellbuf *buf, unsigned int width, unsigned int height);
static void cellbuf_clear(struct cellbuf *buf);
static void cellbuf_free(struct cellbuf *buf);

static void update_term_size();
static int utf8_unicode_to_char(char *out, uint32_t c);
static void send_attr(uint16_t fg, uint16_t bg);
static void send_char(unsigned int x, unsigned int y, uint32_t c);
static void send_clear();
static void sigwinch_handler(int xxx);
static void check_sigwinch();
static int wait_fill_event(struct tb_key_event *event, struct timeval *timeout);

static void fill_inbuf();

/* -------------------------------------------------------- */

int tb_init()
{
	out = fopen("/dev/tty", "w");
	in = fopen("/dev/tty", "r");

	if (!out || !in) 
		return TB_EFAILED_TO_OPEN_TTY;

	out_fileno = fileno(out);
	in_fileno = fileno(in);
	
	if (init_term() < 0)
		return TB_EUNSUPPORTED_TERMINAL;

	signal(SIGWINCH, sigwinch_handler);

	tcgetattr(out_fileno, &orig_tios);
	struct termios tios;
	memset(&tios, 0, sizeof(tios));
	
	tios.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL | IXON);
	tios.c_oflag &= ~OPOST;
	tios.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tios.c_cflag &= ~(CSIZE | PARENB);
	tios.c_cflag |= CS8;
	tios.c_cc[VMIN] = 0;
	tios.c_cc[VTIME] = 0;
	tcsetattr(out_fileno, TCSAFLUSH, &tios);

	fputs(funcs[T_ENTER_CA], out);
	fputs(funcs[T_ENTER_KEYPAD], out);
	fputs(funcs[T_HIDE_CURSOR], out);
	fputs(funcs[T_CLEAR_SCREEN], out);

	fflush(out);
	update_term_size();
	cellbuf_init(&back_buffer, termw, termh);
	cellbuf_init(&front_buffer, termw, termh);
	cellbuf_clear(&back_buffer);
	cellbuf_clear(&front_buffer);
	init_ringbuffer(&inbuf, 4096);

	return 0;
}

void tb_shutdown()
{
	fputs(funcs[T_SHOW_CURSOR], out);
	fputs(funcs[T_SGR0], out);
	fputs(funcs[T_CLEAR_SCREEN], out);
	fputs(funcs[T_EXIT_CA], out);
	fputs(funcs[T_EXIT_KEYPAD], out);
	fflush(out);
	tcsetattr(out_fileno, TCSAFLUSH, &orig_tios);

	fclose(out);
	fclose(in);

	cellbuf_free(&back_buffer);
	cellbuf_free(&front_buffer);
	free_ringbuffer(&inbuf);
}

void tb_present()
{
	unsigned int x,y;
	struct tb_cell *back, *front;

	check_sigwinch();

	for (y = 0; y < front_buffer.height; ++y) {
		back = &CELL(&back_buffer, 0, y);
		front = &CELL(&front_buffer, 0, y);
		for (x = 0; x < front_buffer.width; ++x) {
			back = &CELL(&back_buffer, x, y);
			front = &CELL(&front_buffer, x, y);
			/* what's faster? */
/*			if (*((uint32_t*)back) == *((uint32_t*)front) && 
			    *((uint32_t*)&back->fg) == *((uint32_t*)&front->fg)) */
			if (memcmp(back, front, sizeof(struct tb_cell)) == 0)
				continue;
			send_attr(back->fg, back->bg);
			send_char(x, y, back->ch);
			memcpy(front, back, sizeof(struct tb_cell));
		}
	}
	fflush(out);
}

void tb_put_cell(unsigned int x, unsigned int y, const struct tb_cell *cell)
{
	if (x >= back_buffer.width || y >= back_buffer.height)
		return;
	CELL(&back_buffer, x, y) = *cell;
}

void tb_change_cell(unsigned int x, unsigned int y, uint32_t ch, uint16_t fg, uint16_t bg)
{
	struct tb_cell c = {ch, fg, bg};
	tb_put_cell(x, y, &c);
}

void tb_blit(unsigned int x, unsigned int y, unsigned int w, unsigned int h, const struct tb_cell *cells)
{
	if (x+w >= back_buffer.width || y+h >= back_buffer.height)
		return;

	int sy;
	struct tb_cell *dst = &CELL(&back_buffer, x, y);
	size_t size = sizeof(struct tb_cell) * w;

	for (sy = 0; sy < h; ++sy) {
		memcpy(dst, cells, size);
		dst += back_buffer.width;
		cells += w;
	}
}

int tb_poll_event(struct tb_key_event *event)
{
	return wait_fill_event(event, 0);
}

int tb_peek_event(struct tb_key_event *event, unsigned int timeout)
{
	struct timeval tv;
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout - (tv.tv_sec * 1000)) * 1000;
	return wait_fill_event(event, &tv);
}

unsigned int tb_width() 
{
	return termw;
}

unsigned int tb_height()
{
	return termh;
}

void tb_clear()
{
	check_sigwinch();
	cellbuf_clear(&back_buffer);
}

int tb_select_input_mode(int mode)
{
	if (mode)
		inputmode = mode;
	return inputmode;
}

/* -------------------------------------------------------- */
	
static void cellbuf_init(struct cellbuf *buf, unsigned int width, unsigned int height)
{
	buf->cells = malloc(sizeof(struct tb_cell) * width * height);
	assert(buf->cells);
	buf->width = width;
	buf->height = height;
}

static void cellbuf_resize(struct cellbuf *buf, unsigned int width, unsigned int height)
{
	if (buf->width == width && buf->height == height)
		return;

	unsigned int oldw = buf->width;
	unsigned int oldh = buf->height;
	struct tb_cell *oldcells = buf->cells;

	cellbuf_init(buf, width, height);
	cellbuf_clear(buf);

	unsigned int minw = (width < oldw) ? width : oldw;
	unsigned int minh = (height < oldh) ? height : oldh;
	unsigned int i;

	for (i = 0; i < minh; ++i) {
		struct tb_cell *csrc = oldcells + (i * oldw);
		struct tb_cell *cdst = buf->cells + (i * width);
		memcpy(cdst, csrc, sizeof(struct tb_cell) * minw);
	}

	free(oldcells);
}

static void cellbuf_clear(struct cellbuf *buf)
{
	unsigned int i;
	unsigned int ncells = buf->width * buf->height;

	for (i = 0; i < ncells; ++i) {
		buf->cells[i].ch = ' ';
		buf->cells[i].fg = TB_WHITE;
		buf->cells[i].bg = TB_BLACK;
	}
}

static void cellbuf_free(struct cellbuf *buf)
{
	free(buf->cells);
}

static void update_term_size()
{
	struct winsize sz;
	memset(&sz, 0, sizeof(sz));
	
	ioctl(out_fileno, TIOCGWINSZ, &sz);

	termw = sz.ws_col;
	termh = sz.ws_row;
}

static int utf8_unicode_to_char(char *out, uint32_t c)
{
	int len = 0;    
	int first;
	int i;

	if (c < 0x80) {
		first = 0;
		len = 1;	
	} else if (c < 0x800) {
		first = 0xc0;
		len = 2;
	} else if (c < 0x10000) {
		first = 0xe0;
		len = 3;
	} else if (c < 0x200000) {
		first = 0xf0;
		len = 4;
	} else if (c < 0x4000000) {
		first = 0xf8;
		len = 5;
	} else {
		first = 0xfc;
		len = 6;
	}

	for (i = len - 1; i > 0; --i) {
		out[i] = (c & 0x3f) | 0x80;
		c >>= 6;
	}
	out[0] = c | first;

	return len;
}

static void send_attr(uint16_t fg, uint16_t bg)
{
#define LAST_ATTR_INIT 0xFFFF
	static uint16_t lastfg = LAST_ATTR_INIT, lastbg = LAST_ATTR_INIT;
	if (fg != lastfg || bg != lastbg) {
		fputs(funcs[T_SGR0], out);
		/* TODO: get rid of fprintf */
		fprintf(out, funcs[T_SGR], fg & 0x0F, bg & 0x0F);
		if (fg & TB_BOLD)
			fputs(funcs[T_BOLD], out);
		if (bg & TB_BOLD)
			fputs(funcs[T_BLINK], out);

		lastfg = fg;
		lastbg = bg;
	}
}

static void send_char(unsigned int x, unsigned int y, uint32_t c)
{
#define LAST_COORD_INIT 0xFFFFFFFE
	static int lastx = LAST_COORD_INIT, lasty = LAST_COORD_INIT;
	char buf[7];
	int bw = utf8_unicode_to_char(buf, c);
	buf[bw] = '\0';
	if (((int)x-1) != lastx || y != lasty)
		fprintf(out, funcs[T_MOVE_CURSOR], y+1, x+1); /* TODO: get rid of fprintf */
	lastx = x; lasty = y;
	fputs(buf, out);
}

static void send_clear()
{
	send_attr(TB_WHITE, TB_BLACK);
	fputs(funcs[T_CLEAR_SCREEN], out);
	fflush(out);
}

static void sigwinch_handler(int xxx)
{
	sigwinch_r = 1;
}

static void check_sigwinch()
{
	if (sigwinch_r) {
		update_term_size();
		cellbuf_resize(&back_buffer, termw, termh);
		cellbuf_resize(&front_buffer, termw, termh);
		cellbuf_clear(&front_buffer);

		send_clear();
		
		sigwinch_r = 0;
	}
}

static int wait_fill_event(struct tb_key_event *event, struct timeval *timeout)
{
	int i, result;
	char buf[32];
	fd_set events;
	memset(event, 0, sizeof(struct tb_key_event));

	/* try to extract event from input buffer, return on success */
	if (extract_event(event, &inbuf, inputmode) == 0)
		return 1;

	/* it looks like input buffer is empty, wait for input and fill it */

	while (1) {
		FD_ZERO(&events);
		FD_SET(in_fileno, &events);
		result = select(in_fileno+1, &events, 0, 0, timeout);
		if (!result)
			return 0;

		if (FD_ISSET(in_fileno, &events)) {
			int r = fread(buf, 1, 32, in);
			/* if it's zero read, this is a resize message */
			if (r == 0)
				continue;
			/* if there is no free space in input buffer, return error */
			if (ringbuffer_free_space(&inbuf) < r)
				return -1;
			/* fill buffer */
			ringbuffer_push(&inbuf, buf, r);
			if (extract_event(event, &inbuf, inputmode) == 0)
				return 1;
		}
	}
}


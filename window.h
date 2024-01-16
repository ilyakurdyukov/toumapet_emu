/*
 * Copyright (c) 2024, Ilya Kurdyukov
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>

#ifndef USE_X11
#ifdef __linux__
#define USE_X11 1
#else
#define USE_X11 0
#endif
#endif

#ifndef USE_GDI
#ifdef _WIN32
#define USE_GDI 1
#else
#define USE_GDI 0
#endif
#endif

enum {
	EVENT_EMPTY,
	EVENT_KEY_PRESS,
	EVENT_KEY_RELEASE,
	EVENT_QUIT
};

#if USE_SDL
#if USE_SDL == 1
#include <SDL/SDL.h>
#elif USE_SDL == 2
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

enum {
	SYSKEY_UP = SDLK_UP,
	SYSKEY_DOWN = SDLK_DOWN,
	SYSKEY_LEFT = SDLK_LEFT,
	SYSKEY_RIGHT = SDLK_RIGHT,
	SYSKEY_ESCAPE = SDLK_ESCAPE,
	SYSKEY_DELETE = SDLK_DELETE,
	SYSKEY_PAGEDOWN = SDLK_PAGEDOWN,
	SYSKEY_A = 0
};

typedef struct {
	void *imagedata;
	int w, h, stride, red;
	SDL_Surface *surface;
#if SDL_MAJOR_VERSION >= 2
	SDL_Window *window;
#endif
} window_t;

static void window_close(window_t *x) {
	SDL_Quit();
}

static const char* window_init(window_t *x, const char *name, int w, int h) {
	const char *err;
	x->w = w; x->h = h;
	
	do {
		SDL_Init(SDL_INIT_VIDEO);
#if SDL_MAJOR_VERSION < 2
		x->surface = SDL_SetVideoMode(w, h, 32, SDL_SWSURFACE);
		err = "SDL_SetVideoMode failed";
		if (!x->surface) break;
		SDL_WM_SetCaption(name, 0);
#else
		x->window = SDL_CreateWindow(name,
				SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
				w, h, 0);
		err = "SDL_CreateWindow failed";
		if (!x->window) break;
		x->surface = SDL_GetWindowSurface(x->window);
		err = "SDL_GetWindowSurface failed";
		if (!x->surface) break;
#endif
		x->stride = x->surface->pitch;
		x->imagedata = x->surface->pixels;
		x->red = x->surface->format->Rmask * 513 >> 24 & 3;
		return 0;
	} while (0);
	window_close(x);
	return err;
}

static void window_update(window_t *x) {
#if SDL_MAJOR_VERSION < 2
	SDL_Flip(x->surface);
#else
	SDL_UpdateWindowSurface(x->window);
#endif
}

static int window_event(window_t *x, int *key) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			*key = event.key.keysym.sym;
			return event.type == SDL_KEYDOWN ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;

		case SDL_QUIT:
			return EVENT_QUIT;
		}
	}
	return EVENT_EMPTY;
}

#elif USE_X11
#define _GNU_SOURCE
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <time.h>
#include <sys/time.h>

enum {
	SYSKEY_UP = XK_Up,
	SYSKEY_DOWN = XK_Down,
	SYSKEY_LEFT = XK_Left,
	SYSKEY_RIGHT = XK_Right,
	SYSKEY_ESCAPE = XK_Escape,
	SYSKEY_DELETE = XK_Delete,
	SYSKEY_PAGEDOWN = XK_Page_Down,
	SYSKEY_A = 0
};

typedef struct {
	void *imagedata;
	int screen, w, h, stride, red;
	Window window;
	GC gc;
	Display *display;
	XImage *image;
	Atom wmDelete;
} window_t;

static void window_close(window_t *x) {
	if (x->imagedata) free(x->imagedata);
	if (x->image) {
		x->image->data = NULL;
		XDestroyImage(x->image);
	}
	if (x->window) XDestroyWindow(x->display, x->window);
	if (x->display) XCloseDisplay(x->display);
}

static const char* window_init(window_t *x, const char *name, int w, int h) {
	const char *err; int planes, stride;
	Visual *visual; XSizeHints hints = { 0 };

	x->w = w; x->h = h;

	hints.flags = PMinSize | PMaxSize;
	hints.min_width = hints.max_width = w;
	hints.min_height = hints.max_height = h;

	do {
		x->display = XOpenDisplay(NULL);
		err = "XOpenDisplay failed";
		if (!x->display) break;
		planes = XDisplayPlanes(x->display, x->screen);
		err = "display planes != 24";
		if (planes != 24) break;

		x->gc = XDefaultGC(x->display, x->screen);
		x->window = XCreateSimpleWindow(x->display, RootWindow(x->display, x->screen), 0, 0, w, h, 1, 0, 0);
		err = "XCreateSimpleWindow failed";
		if (!x->window) break;

		XSelectInput(x->display, x->window, KeyPressMask | KeyReleaseMask);

		x->wmDelete = XInternAtom(x->display, "WM_DELETE_WINDOW", 0);
		XSetWMProtocols(x->display, x->window, &x->wmDelete, 1);
		XSetStandardProperties(x->display, x->window, name, NULL, None, NULL, 0, &hints);
		XMapWindow(x->display, x->window);

		visual = XDefaultVisual(x->display, x->screen);
		x->image = XCreateImage(x->display, visual, planes, ZPixmap, 0, 0, w, h, 32, 0);
		err = "XCreateImage failed";
		if (!x->image) break;

		err = "malloc failed";
		x->stride = stride = w * 4;
		x->imagedata = malloc(h * stride);
		if (!x->imagedata) break;
		memset(x->imagedata, 0, h * stride);
		x->image->data = x->imagedata;
		x->red = 2;
		return NULL;
	} while (0);
	window_close(x);
	return err;
}

static void window_update(window_t *x) {
	XPutImage(x->display, x->window, x->gc, x->image, 0, 0, 0, 0, x->w, x->h);
	XSync(x->display, 0);
}

static int window_event(window_t *x, int *key) {
	XEvent event;
	while (XPending(x->display)) {
		XNextEvent(x->display, &event);
		switch (event.type) {
		case KeyPress:
		case KeyRelease:
			*key = XLookupKeysym(&event.xkey, 0);
			return event.type == KeyPress ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;

		case ClientMessage:
			if ((Atom)event.xclient.data.l[0] == x->wmDelete)
				return EVENT_QUIT;
		}
	}
	return EVENT_EMPTY;
}

#elif USE_GDI
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h> /* timeBeginPeriod */

enum {
	SYSKEY_UP = VK_UP,
	SYSKEY_DOWN = VK_DOWN,
	SYSKEY_LEFT = VK_LEFT,
	SYSKEY_RIGHT = VK_RIGHT,
	SYSKEY_ESCAPE = VK_ESCAPE,
	SYSKEY_DELETE = VK_DELETE,
	SYSKEY_PAGEDOWN = VK_NEXT,
	SYSKEY_A = -32
};

typedef struct {
	void *imagedata;
	int w, h, stride, red;
	HWND hwnd; HDC dc;
	BITMAPINFOHEADER bitmap;
} window_t;

static void window_close(window_t *x) {
	if (x->imagedata) free(x->imagedata);
	if (x->dc) ReleaseDC(x->hwnd, x->dc);
	if (x->hwnd) DestroyWindow(x->hwnd);
}

static char window_flags;

static LRESULT CALLBACK windowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
	if (Msg == WM_DESTROY) window_flags = 1;
	return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static const char* window_init(window_t *x, const char *name, int w, int h) {
	const char *err; int stride;
	const char *class_name = "app_window";
	WNDCLASSA wnd_class = { 0, windowProc, 0, 0, NULL, 0, 0, 0, NULL, class_name };
	RECT rect = { 0, 0, w, h };
	int style = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;

	x->w = w; x->h = h;
	window_flags = 0;

	do {
		int ret;
		ATOM class_atom = RegisterClassA(&wnd_class);
		err = "AdjustWindowRect failed";
		if (!class_atom) break;

		ret = AdjustWindowRect(&rect, style, 0);
		err = "AdjustWindowRect failed";
		if (!ret) break;

		x->hwnd = CreateWindowExA(0, class_name, name, style,
				CW_USEDEFAULT, CW_USEDEFAULT,
				rect.right - rect.left, rect.bottom - rect.top, 0, 0, NULL, NULL);
		err = "CreateWindowExA failed";
		if (!x->hwnd) break;
		x->dc = GetDC(x->hwnd);
		err = "GetDC failed";
		if (!x->dc) break;

		err = "malloc failed";
		x->stride = stride = w * 4;
		x->imagedata = malloc(h * stride);
		if (!x->imagedata) break;
		memset(x->imagedata, 0, h * stride);
		x->red = 2;

		timeBeginPeriod(1);
		return NULL;
	} while (0);
	window_close(x);
	return err;
}

static void window_update(window_t *x) {
	BITMAPINFOHEADER bitmap = { 0 };
	bitmap.biSize = sizeof(BITMAPINFOHEADER);
	bitmap.biWidth = x->w;
	bitmap.biHeight = -x->h;
	bitmap.biPlanes = 1;
	bitmap.biBitCount = 32;

	SetDIBitsToDevice(x->dc, 0, 0, x->w, x->h, 0, 0, 0, x->h, x->imagedata, (void*)&bitmap, 0);
	SwapBuffers(x->dc);
}

static int window_event(window_t *x, int *key) {
	MSG msg;
	while (PeekMessageA(&msg, x->hwnd, 0, 0, PM_REMOVE)) {
		switch (msg.message) {
		case WM_KEYDOWN:
		case WM_KEYUP:
			*key = msg.wParam;
			return msg.message == WM_KEYDOWN ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;
		}
		DispatchMessageA(&msg);
	}
	if (window_flags) return EVENT_QUIT;
	return EVENT_EMPTY;
}
#endif


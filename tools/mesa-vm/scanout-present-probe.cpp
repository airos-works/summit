// scanout-present-probe: an EGL/GLES framebuffer object presented straight
// into the screen of a direct window on the X399 (Mesa patch 06,
// summit_haiku_present_framebuffer, and app_server's B_DIRECT_DEVICE_PIXELS).
//
// Renders an animated pattern into a BGRA renderbuffer the size of the
// window in frame buffer pixels, as Summit's compositor does, and copies it
// into the window's visible rectangles with the GPU, into the frame buffer and
// into app_server's copy of the screen. Prints presents per second and what
// each present costs the calling thread; moves the window half way through.
//
//   scanout-present-probe [seconds] [--no-back] [--frame left top right bottom] [--damage n]
//
// Build on the workstation:
//   P=/boot/home/summit-mesa/prefix-20261002
//   g++ -O2 -o scanout-present-probe scanout-present-probe.cpp -I$P/include \
//       -L$P/lib -lEGL -lGLESv2 -lbe -lgame
// Run with LIBRARY_PATH=$P/lib:/boot/system/lib and
// __EGL_VENDOR_LIBRARY_FILENAMES=$P/data/glvnd/egl_vendor.d/50_mesa.json.

#include <Application.h>
#include <Autolock.h>
#include <DirectWindow.h>
#include <Locker.h>
#include <OS.h>
#include <image.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifndef B_DIRECT_DEVICE_PIXELS
#	define B_DIRECT_DEVICE_PIXELS 0x00000400
#endif

// device_scale, drawing_bits_area and drawing_bytes_per_row follow bits_area;
// in headers from before them they are the start of _reserved1. Read by
// position, as WebKitView.cpp does: B_DIRECT_DEVICE_PIXELS is an enumerator
// in Window.h, not a macro, so #ifdef cannot tell the headers apart.
static inline const uint32*
direct_extension(const direct_buffer_info* info)
{
	return reinterpret_cast<const uint32*>(&info->bits_area + 1);
}
#define DEVICE_SCALE(info) (direct_extension(info)[0])
#define DRAWING_AREA(info) ((area_id)direct_extension(info)[1])
#define DRAWING_BPR(info) ((int32)direct_extension(info)[2])

// Mirrors zink_haiku_present.h.
struct summit_haiku_present {
	uint32_t version;
	int32_t origin_x, origin_y;
	uint32_t rect_count;
	const int32_t* rects;
	uint32_t front_bytes_per_row;
	void* back_bits;
	uint32_t back_bytes_per_row;
	uint32_t back_width, back_height;
	uint32_t frames_in_flight;
	void (*completed)(void* cookie);
	void* cookie;
};
typedef int (*present_function)(unsigned, const summit_haiku_present*);
typedef void (*wait_idle_function)();

static present_function sPresent;
static wait_idle_function sWaitIdle;
static bool sUseBack = true;
// Random small rectangles per present, as a page's damage is, instead of
// the whole window (0).
static int sDamage = 0;


static bool
find_present_functions()
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strstr(info.name, "libEGL_mesa") == NULL)
			continue;
		void* present = NULL;
		void* idle = NULL;
		if (get_image_symbol(info.id, "summit_haiku_present_framebuffer",
				B_SYMBOL_TYPE_TEXT, &present) == B_OK
			&& get_image_symbol(info.id, "summit_haiku_present_wait_idle",
				B_SYMBOL_TYPE_TEXT, &idle) == B_OK) {
			sPresent = (present_function)present;
			sWaitIdle = (wait_idle_function)idle;
			printf("present functions from %s\n", info.name);
			return true;
		}
	}
	return false;
}


class ProbeWindow : public BDirectWindow {
public:
	ProbeWindow(BRect frame)
		:
		BDirectWindow(frame, "scanout-present-probe", B_TITLED_WINDOW,
			B_DIRECT_DEVICE_PIXELS | B_QUIT_ON_WINDOW_CLOSE)
	{
	}

	virtual void DirectConnected(direct_buffer_info* info)
	{
		int32 mode = info->buffer_state & B_DIRECT_MODE_MASK;
		BAutolock _(fLock);
		// Nothing of ours may land in rectangles the window no longer owns:
		// the copies already on the GPU finish before app_server goes on.
		if (sWaitIdle != NULL)
			sWaitIdle();
		fConnected = mode != B_DIRECT_STOP && DEVICE_SCALE(info) != 0;
		printf("direct %s scale=%" B_PRIu32 " bounds=(%" B_PRId32 ",%"
			B_PRId32 ")-(%" B_PRId32 ",%" B_PRId32 ") clips=%" B_PRIu32 "\n",
			mode == B_DIRECT_START ? "start" : mode == B_DIRECT_STOP ? "stop"
				: "modify", DEVICE_SCALE(info), info->window_bounds.left,
			info->window_bounds.top, info->window_bounds.right,
			info->window_bounds.bottom, info->clip_list_count);
		if (!fConnected)
			return;
		fOriginX = info->window_bounds.left;
		fOriginY = info->window_bounds.top;
		fWidth = info->window_bounds.right - info->window_bounds.left + 1;
		fHeight = info->window_bounds.bottom - info->window_bounds.top + 1;
		fFrontBytesPerRow = info->bytes_per_row;
		fRects.clear();
		for (uint32 i = 0; i < info->clip_list_count; i++) {
			fRects.push_back(info->clip_list[i].left);
			fRects.push_back(info->clip_list[i].top);
			fRects.push_back(info->clip_list[i].right);
			fRects.push_back(info->clip_list[i].bottom);
		}
		if (DRAWING_AREA(info) != fDrawingSource) {
			if (fDrawingClone >= 0)
				delete_area(fDrawingClone);
			fDrawingSource = DRAWING_AREA(info);
			fDrawingClone = fDrawingSource >= 0
				? clone_area("probe back buffer", &fDrawingBits, B_ANY_ADDRESS,
					B_READ_AREA | B_WRITE_AREA, fDrawingSource) : -1;
			area_info areaInfo;
			fDrawingHeight = fDrawingClone >= 0
				&& get_area_info(fDrawingClone, &areaInfo) == B_OK
				? areaInfo.size / DRAWING_BPR(info) : 0;
		}
		fDrawingBytesPerRow = DRAWING_BPR(info);
	}

	// What the next present may draw, or false when not connected.
	bool Target(summit_haiku_present& present, std::vector<int32_t>& rects,
		int32& width, int32& height)
	{
		BAutolock _(fLock);
		if (!fConnected)
			return false;
		rects = fRects;
		width = fWidth;
		height = fHeight;
		present = summit_haiku_present();
		present.version = 1;
		present.origin_x = fOriginX;
		present.origin_y = fOriginY;
		present.front_bytes_per_row = fFrontBytesPerRow;
		if (sUseBack && fDrawingClone >= 0) {
			present.back_bits = fDrawingBits;
			present.back_bytes_per_row = fDrawingBytesPerRow;
			present.back_width = fDrawingBytesPerRow / 4;
			present.back_height = fDrawingHeight;
		}
		return true;
	}

	BLocker& PresentLock() { return fLock; }

private:
	BLocker		fLock;
	bool		fConnected = false;
	int32		fOriginX = 0, fOriginY = 0, fWidth = 0, fHeight = 0;
	uint32		fFrontBytesPerRow = 0;
	std::vector<int32_t> fRects;
	area_id		fDrawingSource = -1, fDrawingClone = -1;
	void*		fDrawingBits = NULL;
	int32		fDrawingBytesPerRow = 0;
	uint32		fDrawingHeight = 0;
};


int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 6;
	BRect frame(200, 200, 599, 449);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-back"))
			sUseBack = false;
		else if (!strcmp(argv[i], "--damage") && i + 1 < argc)
			sDamage = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--frame") && i + 4 < argc) {
			frame = BRect(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]),
				atof(argv[i + 4]));
			i += 4;
		}
	}
	BApplication app("application/x-vnd.summit-scanout-present-probe");

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major, minor;
	if (!eglInitialize(display, &major, &minor)) {
		printf("eglInitialize failed\n");
		return 1;
	}
	const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint count = 0;
	eglChooseConfig(display, configAttributes, &config, 1, &count);
	const EGLint pbufferAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16,
		EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		pbufferAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE };
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (!eglMakeCurrent(display, surface, surface, context)) {
		printf("eglMakeCurrent failed\n");
		return 1;
	}
	printf("renderer: %s\n", glGetString(GL_RENDERER));
	if (!find_present_functions()) {
		printf("libEGL_mesa has no summit_haiku_present_framebuffer\n");
		return 1;
	}

	ProbeWindow* window = new ProbeWindow(frame);
	window->Show();

	GLuint fbo = 0, color = 0;
	int32 fboWidth = 0, fboHeight = 0;
	bigtime_t start = system_time(), lastReport = start, callTime = 0;
	int frames = 0, failures = 0, lastError = 0;
	bool moved = false;
	while (system_time() - start < seconds * 1000000LL) {
		if (!moved && system_time() - start > seconds * 500000LL) {
			if (window->Lock()) {
				window->MoveBy(120, 60);
				window->Unlock();
			}
			moved = true;
		}
		summit_haiku_present present;
		std::vector<int32_t> rects;
		int32 width, height;
		if (!window->Target(present, rects, width, height)) {
			snooze(5000);
			continue;
		}
		if (width != fboWidth || height != fboHeight) {
			if (fbo != 0) {
				glDeleteFramebuffers(1, &fbo);
				glDeleteRenderbuffers(1, &color);
			}
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glGenRenderbuffers(1, &color);
			glBindRenderbuffer(GL_RENDERBUFFER, color);
			glRenderbufferStorage(GL_RENDERBUFFER, 0x93A1 /* GL_BGRA8_EXT */,
				width, height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_RENDERBUFFER, color);
			printf("framebuffer %dx%d: %s\n", (int)width, (int)height,
				glCheckFramebufferStatus(GL_FRAMEBUFFER)
					== GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
			fboWidth = width;
			fboHeight = height;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.1f, 0.2f, 0.6f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		// Quadrants (GL rows count from the top in this target, as in
		// WebKit's compositor), and a bar sweeping across.
		glScissor(0, 0, width / 2, height / 2);
		glClearColor(0.9f, 0.8f, 0.15f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor(width / 2, height / 2, width - width / 2, height - height / 2);
		glClearColor(0.12f, 0.24f, 0.16f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor((frames * 8) % width, 0, 24, height);
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glScissor(0, 0, width, 2);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);

		if (sDamage > 0 && !rects.empty()) {
			// Odd sizes at odd places inside the first visible rectangle.
			int32_t l = rects[0], t = rects[1], r = rects[2], b = rects[3];
			std::vector<int32_t> damage;
			for (int i = 0; i < sDamage; i++) {
				int32_t x = l + rand() % std::max(1, (int)(r - l + 1));
				int32_t y = t + rand() % std::max(1, (int)(b - t + 1));
				int32_t w = 1 + rand() % 200, h = 1 + rand() % 120;
				damage.push_back(x);
				damage.push_back(y);
				damage.push_back(std::min(r, x + w - 1));
				damage.push_back(std::min(b, y + h - 1));
			}
			rects = damage;
		}
		present.rect_count = rects.size() / 4;
		present.rects = rects.data();
		bigtime_t before = system_time();
		int result;
		{
			// Holding the window's lock keeps DirectConnected() from
			// changing the rectangles while they are being copied.
			BAutolock _(window->PresentLock());
			result = sPresent(fbo, &present);
		}
		callTime += system_time() - before;
		if (result != 0) {
			failures++;
			lastError = result;
		}
		frames++;
		if (system_time() - lastReport >= 1000000) {
			bigtime_t span = system_time() - lastReport;
			printf("%.1f presents/s, %.2f ms per call, failures %d (last %d)\n",
				frames * 1e6 / span, frames ? callTime / 1000.0 / frames : 0,
				failures, lastError);
			fflush(stdout);
			frames = failures = 0;
			callTime = 0;
			lastReport = system_time();
		}
	}
	if (window->Lock())
		window->Quit();
	sWaitIdle();
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(display);
	return 0;
}

#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <windows.h>
#include <dxgi.h>
#include <ipc-util/pipe.h>
#include "obfuscate.h"
#include "graphics-hook-info.h"
#include "window-helpers.h"
#include "cursor-capture.h"

#define do_log(level, format, ...) \
	blog(level, "[game-capture: '%s'] " format, \
			obs_source_get_name(gc->source), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define SETTING_ANY_FULLSCREEN   "capture_any_fullscreen"
#define SETTING_CAPTURE_WINDOW   "window"
#define SETTING_WINDOW_PRIORITY  "priority"
#define SETTING_ACTIVATE_HOOK    "activate_hook"
#define SETTING_COMPATIBILITY    "sli_compatibility"
#define SETTING_FORCE_SCALING    "force_scaling"
#define SETTING_SCALE_RES        "scale_res"
#define SETTING_CURSOR           "capture_cursor"
#define SETTING_TRANSPARENCY     "allow_transparency"
#define SETTING_LIMIT_FRAMERATE  "limit_framerate"
#define SETTING_CAPTURE_OVERLAYS "capture_overlays"

#define TEXT_GAME_CAPTURE        obs_module_text("GameCapture")
#define TEXT_ANY_FULLSCREEN      obs_module_text("GameCapture.AnyFullscreen")
#define TEXT_ACTIVATE_HOOK       obs_module_text("GameCapture.Activate")
#define TEXT_SLI_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_ALLOW_TRANSPARENCY  obs_module_text("AllowTransparency")
#define TEXT_FORCE_SCALING       obs_module_text("GameCapture.ForceScaling")
#define TEXT_SCALE_RES           obs_module_text("GameCapture.ScaleRes")
#define TEXT_WINDOW              obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY      obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE         obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS         obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE           obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR      obs_module_text("CaptureCursor")
#define TEXT_LIMIT_FRAMERATE     obs_module_text("GameCapture.LimitFramerate")
#define TEXT_CAPTURE_OVERLAYS    obs_module_text("GameCapture.CaptureOverlays")

struct game_capture_config {
	char                          *title;
	char                          *class;
	char                          *executable;
	enum window_priority          priority;
	uint32_t                      scale_cx;
	uint32_t                      scale_cy;
	bool                          cursor : 1;
	bool                          force_shmem : 1;
	bool                          capture_any_fullscreen : 1;
	bool                          force_scaling : 1;
	bool                          allow_transparency : 1;
	bool                          limit_framerate : 1;
	bool                          capture_overlays : 1;
};

struct game_capture {
	obs_source_t                  *source;

	struct cursor_data            cursor_data;
	HANDLE                        injector_process;
	uint32_t                      cx;
	uint32_t                      cy;
	uint32_t                      pitch;
	DWORD                         process_id;
	DWORD                         thread_id;
	HWND                          next_window;
	HWND                          window;
	float                         check_interval;
	float                         fps_reset_interval;
	bool                          active : 1;
	bool                          activate_hook : 1;
	bool                          process_is_64bit : 1;
	bool                          error_aqcuiring : 1;
	bool                          dwm_capture : 1;
	bool                          initial_config : 1;

	struct game_capture_config    config;

	ipc_pipe_server_t             pipe;
	gs_texture_t                  *texture;
	struct hook_info              *global_hook_info;
	HANDLE                        keep_alive;
	HANDLE                        hook_restart;
	HANDLE                        hook_stop;
	HANDLE                        hook_ready;
	HANDLE                        hook_exit;
	HANDLE                        hook_data_map;
	HANDLE                        global_hook_info_map;
	HANDLE                        target_process;
	HANDLE                        texture_mutexes[2];

	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *texture_buffers[2];
		};

		struct shtex_data *shtex_data;
		void *data;
	};

	void (*copy_texture)(struct game_capture*);
};

struct graphics_offsets offsets32 = {0};
struct graphics_offsets offsets64 = {0};

static inline enum gs_color_format convert_format(uint32_t format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:     return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_UNORM:     return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_UNORM:     return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:  return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM: return GS_RGBA16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return GS_RGBA32F;
	}

	return GS_UNKNOWN;
}

static void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}

static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle,
		DWORD process_id)
{
	static HANDLE (WINAPI *open_process_proc)(DWORD, BOOL, DWORD) = NULL;
	if (!open_process_proc)
		open_process_proc = get_obfuscated_func(kernel32(),
				"NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

static void stop_capture(struct game_capture *gc)
{
	ipc_pipe_server_free(&gc->pipe);

	if (gc->hook_stop) {
		SetEvent(gc->hook_stop);
	}
	if (gc->global_hook_info) {
		UnmapViewOfFile(gc->global_hook_info);
		gc->global_hook_info = NULL;
	}
	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	close_handle(&gc->keep_alive);
	close_handle(&gc->hook_restart);
	close_handle(&gc->hook_stop);
	close_handle(&gc->hook_ready);
	close_handle(&gc->hook_exit);
	close_handle(&gc->hook_data_map);
	close_handle(&gc->global_hook_info_map);
	close_handle(&gc->target_process);
	close_handle(&gc->texture_mutexes[0]);
	close_handle(&gc->texture_mutexes[1]);

	if (gc->texture) {
		obs_enter_graphics();
		gs_texture_destroy(gc->texture);
		obs_leave_graphics();
		gc->texture = NULL;
	}

	gc->copy_texture = NULL;
	gc->active = false;
}

static inline void free_config(struct game_capture_config *config)
{
	bfree(config->title);
	bfree(config->class);
	bfree(config->executable);
	memset(config, 0, sizeof(*config));
}

static void game_capture_destroy(void *data)
{
	struct game_capture *gc = data;
	stop_capture(gc);

	obs_enter_graphics();
	cursor_data_free(&gc->cursor_data);
	obs_leave_graphics();

	free_config(&gc->config);
	bfree(gc);
}

static inline void get_config(struct game_capture_config *cfg,
		obs_data_t *settings)
{
	int ret;
	const char *scale_str;
	const char *window = obs_data_get_string(settings,
			SETTING_CAPTURE_WINDOW);

	build_window_strings(window, &cfg->class, &cfg->title,
			&cfg->executable);

	cfg->capture_any_fullscreen = obs_data_get_bool(settings,
			SETTING_ANY_FULLSCREEN);
	cfg->priority = (enum window_priority)obs_data_get_int(settings,
			SETTING_WINDOW_PRIORITY);
	cfg->force_shmem = obs_data_get_bool(settings,
			SETTING_COMPATIBILITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency = obs_data_get_bool(settings,
			SETTING_TRANSPARENCY);
	cfg->force_scaling = obs_data_get_bool(settings,
			SETTING_FORCE_SCALING);
	cfg->limit_framerate = obs_data_get_bool(settings,
			SETTING_LIMIT_FRAMERATE);
	cfg->capture_overlays = obs_data_get_bool(settings,
			SETTING_CAPTURE_OVERLAYS);

	scale_str = obs_data_get_string(settings, SETTING_SCALE_RES);
	ret = sscanf(scale_str, "%"PRIu32"x%"PRIu32,
			&cfg->scale_cx, &cfg->scale_cy);

	cfg->scale_cx &= ~2;
	cfg->scale_cy &= ~2;

	if (cfg->force_scaling) {
		if (ret != 2 || cfg->scale_cx == 0 || cfg->scale_cy == 0) {
			cfg->scale_cx = 0;
			cfg->scale_cy = 0;
		}
	}
}

static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool capture_needs_reset(struct game_capture_config *cfg1,
		struct game_capture_config *cfg2)
{
	if (cfg1->capture_any_fullscreen != cfg2->capture_any_fullscreen) {
		return true;

	} else if (!cfg1->capture_any_fullscreen &&
			(s_cmp(cfg1->class, cfg2->class) != 0 ||
			 s_cmp(cfg1->title, cfg2->title) != 0 ||
			 s_cmp(cfg1->executable, cfg2->executable) != 0 ||
			 cfg1->priority != cfg2->priority)) {
		return true;

	} else if (cfg1->force_scaling != cfg2->force_scaling) {
		return true;

	} else if (cfg1->force_scaling &&
			(cfg1->scale_cx != cfg2->scale_cx ||
			 cfg1->scale_cy != cfg2->scale_cy)) {
		return true;

	} else if (cfg1->force_shmem != cfg2->force_shmem) {
		return true;

	} else if (cfg1->limit_framerate != cfg2->limit_framerate) {
		return true;

	} else if (cfg1->capture_overlays != cfg2->capture_overlays) {
		return true;
	}

	return false;
}

static void game_capture_update(void *data, obs_data_t *settings)
{
	struct game_capture *gc = data;
	struct game_capture_config cfg;
	bool reset_capture = false;

	get_config(&cfg, settings);
	reset_capture = capture_needs_reset(&cfg, &gc->config);

	if (cfg.force_scaling && (cfg.scale_cx == 0 || cfg.scale_cy == 0)) {
		gc->error_aqcuiring = true;
	} else {
		gc->error_aqcuiring = false;
	}

	free_config(&gc->config);
	gc->config = cfg;
	gc->activate_hook = obs_data_get_bool(settings, "activate_hook");

	if (!gc->initial_config) {
		if (reset_capture) {
			gc->activate_hook = false;
			obs_data_set_bool(settings, "activate_hook", false);
			stop_capture(gc);
		}
	} else {
		gc->initial_config = false;
	}
}

static void *game_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct game_capture *gc = bzalloc(sizeof(*gc));
	gc->source = source;
	gc->initial_config = true;

	game_capture_update(gc, settings);
	return gc;
}

static inline HANDLE create_event_id(bool manual_reset, bool initial_state,
		const char *name, DWORD process_id)
{
	char new_name[128];
	sprintf(new_name, "%s%d", name, process_id);
	return CreateEventA(NULL, manual_reset, initial_state, new_name);
}

static inline HANDLE open_event_id(const char *name, DWORD process_id)
{
	char new_name[128];
	sprintf(new_name, "%s%d", name, process_id);
	return OpenEventA(EVENT_ALL_ACCESS, false, new_name);
}

#define STOP_BEING_BAD \
	"  This is most likely due to security software. Please make sure " \
        "that the OBS installation folder is excluded/ignored in the "      \
        "settings of the security software you are using."

static bool check_file_integrity(struct game_capture *gc, const char *file,
		const char *name)
{
	DWORD error;
	HANDLE handle;

	if (!file || !*file) {
		warn("Game capture %s not found." STOP_BEING_BAD, name);
		return false;
	}

	handle = CreateFileA(file, GENERIC_READ | GENERIC_EXECUTE,
			FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	if (handle != INVALID_HANDLE_VALUE) {
		CloseHandle(handle);
		return true;
	}

	error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) {
		warn("Game capture file '%s' not found."
				STOP_BEING_BAD, file);
	} else if (error == ERROR_ACCESS_DENIED) {
		warn("Game capture file '%s' could not be loaded."
				STOP_BEING_BAD, file);
	} else {
		warn("Game capture file '%s' could not be loaded: %lu."
				STOP_BEING_BAD, file, error);
	}

	return false;
}

static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}

static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}

static inline bool open_target_process(struct game_capture *gc)
{
	gc->target_process = open_process(
			PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
			false, gc->process_id);
	if (!gc->target_process) {
		warn("could not open process: %s", gc->config.executable);
		return false;
	}

	gc->process_is_64bit = is_64bit_process(gc->target_process);
	return true;
}

static inline bool init_keepalive(struct game_capture *gc)
{
	gc->keep_alive = create_event_id(false, false, EVENT_HOOK_KEEPALIVE,
			gc->process_id);
	if (!gc->keep_alive) {
		warn("failed to create keepalive event");
		return false;
	}

	return true;
}

static inline bool init_texture_mutexes(struct game_capture *gc)
{
	gc->texture_mutexes[0] = get_mutex_plus_id(MUTEX_TEXTURE1,
			gc->process_id);
	gc->texture_mutexes[1] = get_mutex_plus_id(MUTEX_TEXTURE2,
			gc->process_id);

	if (!gc->texture_mutexes[0] || !gc->texture_mutexes[1]) {
		warn("failed to create texture mutexes: %lu", GetLastError());
		return false;
	}

	return true;
}

/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct game_capture *gc)
{
	gc->hook_restart = open_event_id(EVENT_CAPTURE_RESTART, gc->process_id);
	if (gc->hook_restart) {
		debug("existing hook found, signaling process: %s",
				gc->config.executable);
		SetEvent(gc->hook_restart);
		return true;
	}

	return false;
}

static inline void reset_frame_interval(struct game_capture *gc)
{
	struct obs_video_info ovi;
	uint64_t interval = 0;

	if (gc->config.limit_framerate && obs_get_video_info(&ovi))
		interval = ovi.fps_den * 1000000000ULL / ovi.fps_num;

	gc->global_hook_info->frame_interval = interval;
}

static inline bool init_hook_info(struct game_capture *gc)
{
	gc->global_hook_info_map = get_hook_info(gc->process_id);
	if (!gc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu",
				GetLastError());
		return false;
	}

	gc->global_hook_info = MapViewOfFile(gc->global_hook_info_map,
			FILE_MAP_ALL_ACCESS, 0, 0,
			sizeof(*gc->global_hook_info));
	if (!gc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu",
				GetLastError());
		return false;
	}

	gc->global_hook_info->offsets = gc->process_is_64bit ?
		offsets64 : offsets32;
	gc->global_hook_info->capture_overlay = gc->config.capture_overlays;
	gc->global_hook_info->force_shmem = gc->config.force_shmem;
	gc->global_hook_info->use_scale = gc->config.force_scaling;
	gc->global_hook_info->cx = gc->config.scale_cx;
	gc->global_hook_info->cy = gc->config.scale_cy;
	reset_frame_interval(gc);

	obs_enter_graphics();
	if (!gs_shared_texture_available())
		gc->global_hook_info->force_shmem = true;
	obs_leave_graphics();

	obs_enter_graphics();
	if (!gs_shared_texture_available())
		gc->global_hook_info->force_shmem = true;
	obs_leave_graphics();

	return true;
}

static void pipe_log(void *param, uint8_t *data, size_t size)
{
	struct game_capture *gc = param;
	if (data && size)
		info("%s", data);
}

static inline bool init_pipe(struct game_capture *gc)
{
	char name[64];
	sprintf(name, "%s%d", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	return true;
}

static inline bool create_inject_process(struct game_capture *gc,
		const char *inject_path, const char *hook_path)
{
	wchar_t *command_line_w = malloc(4096 * sizeof(wchar_t));
	wchar_t *inject_path_w;
	wchar_t *hook_path_w;
	PROCESS_INFORMATION pi = {0};
	STARTUPINFO si = {0};
	bool success = false;

	os_utf8_to_wcs_ptr(inject_path, 0, &inject_path_w);
	os_utf8_to_wcs_ptr(hook_path, 0, &hook_path_w);

	si.cb = sizeof(si);

	swprintf(command_line_w, 4096, L"\"%s\" \"%s\" %lu",
			inject_path_w, hook_path_w, gc->thread_id);

	success = !!CreateProcessW(inject_path_w, command_line_w, NULL, NULL,
			false, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (success) {
		CloseHandle(pi.hThread);
		gc->injector_process = pi.hProcess;
	} else {
		warn("Failed to create inject helper process: %lu",
				GetLastError());
	}

	free(command_line_w);
	bfree(inject_path_w);
	bfree(hook_path_w);
	return success;
}

static inline bool inject_hook(struct game_capture *gc)
{
	bool success = false;
	char *inject_path;
	char *hook_path;

	if (gc->process_is_64bit) {
		inject_path = obs_module_file("inject-helper64.exe");
		hook_path = obs_module_file("graphics-hook64.dll");
	} else {
		inject_path = obs_module_file("inject-helper32.exe");
		hook_path = obs_module_file("graphics-hook32.dll");
	}

	if (!check_file_integrity(gc, inject_path, "inject helper")) {
		goto cleanup;
	}
	if (!check_file_integrity(gc, hook_path, "graphics hook")) {
		goto cleanup;
	}

	success = create_inject_process(gc, inject_path, hook_path);

cleanup:
	bfree(inject_path);
	bfree(hook_path);
	return success;
}

static bool init_hook(struct game_capture *gc)
{
	if (gc->config.capture_any_fullscreen) {
		struct dstr name = {0};
		if (get_window_exe(&name, gc->next_window)) {
			info("attempting to hook fullscreen process: %s",
					name.array);
			dstr_free(&name);
		}
	} else {
		info("attempting to hook process: %s", gc->config.executable);
	}

	if (!open_target_process(gc)) {
		return false;
	}
	if (!init_keepalive(gc)) {
		return false;
	}
	if (!init_texture_mutexes(gc)) {
		return false;
	}
	if (!init_hook_info(gc)) {
		return false;
	}
	if (!init_pipe(gc)) {
		return false;
	}
	if (!attempt_existing_hook(gc)) {
		if (!inject_hook(gc)) {
			return false;
		}
	}

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;
	return true;
}

static void get_fullscreen_window(struct game_capture *gc)
{
	HWND window = GetForegroundWindow();
	MONITORINFO mi = {0};
	HMONITOR monitor;
	DWORD styles;
	RECT rect;

	gc->next_window = NULL;

	if (!window) {
		return;
	}
	if (!GetWindowRect(window, &rect)) {
		return;
	}

	/* ignore regular maximized windows */
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	if ((styles & WS_MAXIMIZE) != 0 && (styles & WS_BORDER) != 0) {
		return;
	}

	monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	if (!monitor) {
		return;
	}

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(monitor, &mi)) {
		return;
	}

	if (rect.left   == mi.rcMonitor.left   &&
	    rect.right  == mi.rcMonitor.right  &&
	    rect.bottom == mi.rcMonitor.bottom &&
	    rect.top    == mi.rcMonitor.top) {
		gc->next_window = window;
	}
}

static void get_selected_window(struct game_capture *gc)
{
	if (strcmpi(gc->config.class, "dwm") == 0) {
		wchar_t class_w[512];
		os_utf8_to_wcs(gc->config.class, 0, class_w, 512);
		gc->next_window = FindWindowW(class_w, NULL);
	} else {
		gc->next_window = find_window(INCLUDE_MINIMIZED,
				gc->config.priority,
				gc->config.class,
				gc->config.title,
				gc->config.executable);
	}
}

static void try_hook(struct game_capture *gc)
{
	if (gc->config.capture_any_fullscreen) {
		get_fullscreen_window(gc);
	} else {
		get_selected_window(gc);
	}

	if (gc->next_window) {
		gc->thread_id = GetWindowThreadProcessId(gc->next_window,
				&gc->process_id);

		if (!gc->thread_id || !gc->process_id) {
			warn("failed to get window thread/process ids: %d",
					GetLastError());
			gc->error_aqcuiring = true;
			return;
		}

		if (!init_hook(gc)) {
			stop_capture(gc);
		}
	} else {
		gc->active = false;
	}
}

static inline bool init_events(struct game_capture *gc)
{
	if (!gc->hook_restart) {
		gc->hook_restart = get_event_plus_id(EVENT_CAPTURE_RESTART,
				gc->process_id);
		if (!gc->hook_restart) {
			warn("init_events: failed to get hook_restart "
			     "event: %lu", GetLastError());
			return false;
		}
	}

	gc->hook_stop = get_event_plus_id(EVENT_CAPTURE_STOP, gc->process_id);
	if (!gc->hook_stop) {
		warn("init_events: failed to get hook_stop event: %lu",
				GetLastError());
		return false;
	}

	gc->hook_ready = get_event_plus_id(EVENT_HOOK_READY, gc->process_id);
	if (!gc->hook_ready) {
		warn("init_events: failed to get hook_ready event: %lu",
				GetLastError());
		return false;
	}

	gc->hook_exit = get_event_plus_id(EVENT_HOOK_EXIT, gc->process_id);
	if (!gc->hook_exit) {
		warn("init_events: failed to get hook_exit event: %lu",
				GetLastError());
		return false;
	}

	return true;
}

static inline bool init_capture_data(struct game_capture *gc)
{
	char name[64];
	sprintf(name, "%s%u", SHMEM_TEXTURE, gc->global_hook_info->map_id);

	gc->cx = gc->global_hook_info->cx;
	gc->cy = gc->global_hook_info->cy;
	gc->pitch = gc->global_hook_info->pitch;

	gc->hook_data_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, name);
	if (!gc->hook_data_map) {
		warn("init_capture_data: failed to open file mapping: %lu",
				GetLastError());
		return false;
	}

	gc->data = MapViewOfFile(gc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0,
			gc->global_hook_info->map_size);
	if (!gc->data) {
		warn("init_capture_data: failed to map data view: %lu",
				GetLastError());
		return false;
	}

	return true;
}

static void copy_shmem_tex(struct game_capture *gc)
{
	int cur_texture = gc->shmem_data->last_tex;
	HANDLE mutex = NULL;
	uint32_t pitch;
	int next_texture;
	uint8_t *data;

	if (cur_texture < 0 || cur_texture > 1)
		return;

	next_texture = cur_texture == 1 ? 0 : 1;

	if (object_signalled(gc->texture_mutexes[cur_texture])) {
		mutex = gc->texture_mutexes[cur_texture];

	} else if (object_signalled(gc->texture_mutexes[next_texture])) {
		mutex = gc->texture_mutexes[next_texture];
		cur_texture = next_texture;

	} else {
		return;
	}

	if (gs_texture_map(gc->texture, &data, &pitch)) {
		if (pitch == gc->pitch) {
			memcpy(data, gc->texture_buffers[cur_texture],
					pitch * gc->cy);
		} else {
			uint8_t *input = gc->texture_buffers[cur_texture];
			uint32_t best_pitch =
				pitch < gc->pitch ? pitch : gc->pitch;

			for (uint32_t y = 0; y < gc->cy; y++) {
				uint8_t *line_in = input + gc->pitch * y;
				uint8_t *line_out = data + pitch * y;
				memcpy(line_out, line_in, best_pitch);
			}
		}

		gs_texture_unmap(gc->texture);
	}

	ReleaseMutex(mutex);
}

static inline bool init_shmem_capture(struct game_capture *gc)
{
	gc->texture_buffers[0] =
		(uint8_t*)gc->data + gc->shmem_data->tex1_offset;
	gc->texture_buffers[1] =
		(uint8_t*)gc->data + gc->shmem_data->tex2_offset;

	obs_enter_graphics();
	gc->texture = gs_texture_create(gc->cx, gc->cy,
			convert_format(gc->global_hook_info->format),
			1, NULL, GS_DYNAMIC);
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shmem_capture: failed to create texture");
		return false;
	}

	gc->copy_texture = copy_shmem_tex;
	return true;
}

static inline bool init_shtex_capture(struct game_capture *gc)
{
	obs_enter_graphics();
	gc->texture = gs_texture_open_shared(gc->shtex_data->tex_handle);
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shtex_capture: failed to open shared handle");
		return false;
	}

	return true;
}

static bool start_capture(struct game_capture *gc)
{
	if (!init_events(gc)) {
		return false;
	}
	if (!init_capture_data(gc)) {
		return false;
	}
	if (gc->global_hook_info->type == CAPTURE_TYPE_MEMORY) {
		if (!init_shmem_capture(gc)) {
			return false;
		}
	} else {
		if (!init_shtex_capture(gc)) {
			return false;
		}
	}

	return true;
}

static void game_capture_tick(void *data, float seconds)
{
	struct game_capture *gc = data;

	if (gc->hook_stop && object_signalled(gc->hook_stop)) {
		stop_capture(gc);
	}

	if (gc->active && !gc->hook_ready && gc->process_id) {
		gc->hook_ready = get_event_plus_id(EVENT_HOOK_READY,
				gc->process_id);
	}

	if (gc->injector_process && object_signalled(gc->injector_process)) {
		DWORD exit_code = 0;

		GetExitCodeProcess(gc->injector_process, &exit_code);
		close_handle(&gc->injector_process);

		if (exit_code != 0) {
			warn("inject process failed: %d", exit_code);
			gc->error_aqcuiring = true;
		}
	}

	if (gc->hook_ready && object_signalled(gc->hook_ready)) {
		if (!start_capture(gc)) {
			stop_capture(gc);
			gc->error_aqcuiring = true;
		}
	}

	gc->check_interval += seconds;

	if (!gc->active) {
		if (!gc->error_aqcuiring && gc->check_interval > 3.0f) {
			if (gc->config.capture_any_fullscreen ||
			    gc->activate_hook) {
				try_hook(gc);
				gc->check_interval = 0.0f;
			}
		}
	} else {
		if (!IsWindow(gc->window) && !gc->dwm_capture ||
		    object_signalled(gc->target_process)) {
			info("capture window no longer exists, "
			     "terminating capture");
			stop_capture(gc);
		} else {
			if (gc->copy_texture) {
				obs_enter_graphics();
				gc->copy_texture(gc);
				obs_leave_graphics();
			}

			if (gc->config.cursor) {
				obs_enter_graphics();
				cursor_capture(&gc->cursor_data);
				obs_leave_graphics();
			}

			gc->fps_reset_interval += seconds;
			if (gc->fps_reset_interval >= 3.0f) {
				reset_frame_interval(gc);
				gc->fps_reset_interval = 0.0f;
			}
		}
	}
}

static inline void game_capture_render_cursor(struct game_capture *gc)
{
	POINT p = {0};

	if (!gc->global_hook_info->window ||
	    !gc->global_hook_info->base_cx ||
	    !gc->global_hook_info->base_cy)
		return;

	ClientToScreen((HWND)gc->global_hook_info->window, &p);

	float x_scale = (float)gc->global_hook_info->cx /
		(float)gc->global_hook_info->base_cx;
	float y_scale = (float)gc->global_hook_info->cy /
		(float)gc->global_hook_info->base_cy;

	cursor_draw(&gc->cursor_data, -p.x, -p.y, x_scale, y_scale,
			gc->global_hook_info->base_cx,
			gc->global_hook_info->base_cy);
}

static void game_capture_render(void *data, gs_effect_t *effect)
{
	struct game_capture *gc = data;
	if (!gc->texture)
		return;

	effect = obs_get_default_effect();

	while (gs_effect_loop(effect, "Draw")) {
		if (!gc->config.allow_transparency) {
			gs_enable_blending(false);
			gs_enable_color(true, true, true, false);
		}

		obs_source_draw(gc->texture, 0, 0, 0, 0,
				gc->global_hook_info->flip);

		if (!gc->config.allow_transparency) {
			gs_enable_blending(true);
			gs_enable_color(true, true, true, true);
		}

		if (gc->config.cursor) {
			game_capture_render_cursor(gc);
		}
	}
}

static uint32_t game_capture_width(void *data)
{
	struct game_capture *gc = data;
	return gc->active ? gc->global_hook_info->cx : 0;
}

static uint32_t game_capture_height(void *data)
{
	struct game_capture *gc = data;
	return gc->active ? gc->global_hook_info->cy : 0;
}

static const char *game_capture_name(void)
{
	return TEXT_GAME_CAPTURE;
}

static void game_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_ANY_FULLSCREEN, true);
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY,
			(int)WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_COMPATIBILITY, false);
	obs_data_set_default_bool(settings, SETTING_FORCE_SCALING, false);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
	obs_data_set_default_string(settings, SETTING_SCALE_RES, "0x0");
	obs_data_set_default_bool(settings, SETTING_LIMIT_FRAMERATE, false);
	obs_data_set_default_bool(settings, SETTING_CAPTURE_OVERLAYS, false);
}

static bool activate_clicked(obs_properties_t *props, obs_property_t *property,
		void *data)
{
	struct game_capture *gc = data;
	obs_data_t *settings = obs_source_get_settings(gc->source);

	gc->activate_hook = true;
	obs_data_set_bool(settings, "activate_hook", true);
	obs_data_release(settings);

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	return false;
}

static bool any_fullscreen_callback(obs_properties_t *ppts,
		obs_property_t *p, obs_data_t *settings)
{
	bool any_fullscreen = obs_data_get_bool(settings,
			SETTING_ANY_FULLSCREEN);

	p = obs_properties_get(ppts, SETTING_CAPTURE_WINDOW);
	obs_property_set_enabled(p, !any_fullscreen);

	p = obs_properties_get(ppts, SETTING_WINDOW_PRIORITY);
	obs_property_set_enabled(p, !any_fullscreen);

	p = obs_properties_get(ppts, SETTING_ACTIVATE_HOOK);
	obs_property_set_enabled(p, !any_fullscreen);
	return true;
}

static bool use_scaling_callback(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	bool use_scale = obs_data_get_bool(settings, SETTING_FORCE_SCALING);

	p = obs_properties_get(ppts, SETTING_SCALE_RES);
	obs_property_set_enabled(p, use_scale);
	return true;
}

static void insert_preserved_val(obs_property_t *p, const char *val)
{
	char *class = NULL;
	char *title = NULL;
	char *executable = NULL;
	struct dstr desc = {0};

	build_window_strings(val, &class, &title, &executable);

	dstr_printf(&desc, "[%s]: %s", executable, title);
	obs_property_list_insert_string(p, 0, desc.array, val);
	obs_property_list_item_disable(p, 0, true);

	dstr_free(&desc);
	bfree(class);
	bfree(title);
	bfree(executable);
}

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	const char *first_val = obs_property_list_item_string(p, 0);
	const char *cur_val;
	bool match = false;
	size_t i = 0;

	cur_val = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);
	if (!cur_val) {
		return false;
	}

	for (;;) {
		const char *val = obs_property_list_item_string(p, i++);
		if (!val || !*val)
			break;

		if (strcmp(val, cur_val) == 0) {
			match = true;
			break;
		}
	}

	if (cur_val && first_val && *cur_val && *first_val && !match) {
		insert_preserved_val(p, cur_val);
		return true;
	}

	return false;
}

static const double default_scale_vals[] = {
	1.25,
	1.5,
	2.0,
	2.5,
	3.0
};

#define NUM_DEFAULT_SCALE_VALS \
	(sizeof(default_scale_vals) / sizeof(default_scale_vals[0]))

static BOOL CALLBACK EnumFirstMonitor(HMONITOR monitor, HDC hdc,
		LPRECT rc, LPARAM data)
{
	*(HMONITOR*)data = monitor;
	return false;
}

static obs_properties_t *game_capture_properties(void *data)
{
	struct game_capture *gc = data;
	HMONITOR monitor;
	uint32_t cx = 1920;
	uint32_t cy = 1080;

	/* scaling is free form, this is mostly just to provide some common
	 * values */
	bool success = !!EnumDisplayMonitors(NULL, NULL, EnumFirstMonitor,
			(LPARAM)&monitor);
	if (success) {
		MONITORINFO mi = {0};
		mi.cbSize = sizeof(mi);

		if (!!GetMonitorInfo(monitor, &mi)) {
			cx = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			cy = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(ppts, SETTING_ANY_FULLSCREEN,
			TEXT_ANY_FULLSCREEN);

	obs_property_set_modified_callback(p, any_fullscreen_callback);

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_WINDOW, TEXT_WINDOW,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_window_list(p, INCLUDE_MINIMIZED);

	obs_property_set_modified_callback(p, window_changed_callback);

	p = obs_properties_add_list(ppts, SETTING_WINDOW_PRIORITY,
			TEXT_MATCH_PRIORITY, OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE,   WINDOW_PRIORITY_EXE);

	obs_properties_add_button(ppts, SETTING_ACTIVATE_HOOK,
			TEXT_ACTIVATE_HOOK, activate_clicked);

	obs_properties_add_bool(ppts, SETTING_COMPATIBILITY,
			TEXT_SLI_COMPATIBILITY);

	p = obs_properties_add_bool(ppts, SETTING_FORCE_SCALING,
			TEXT_FORCE_SCALING);

	obs_property_set_modified_callback(p, use_scaling_callback);

	p = obs_properties_add_list(ppts, SETTING_SCALE_RES, TEXT_SCALE_RES,
			OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	for (size_t i = 0; i < NUM_DEFAULT_SCALE_VALS; i++) {
		char scale_str[64];
		uint32_t new_cx =
			(uint32_t)((double)cx / default_scale_vals[i]) & ~2;
		uint32_t new_cy =
			(uint32_t)((double)cy / default_scale_vals[i]) & ~2;

		sprintf(scale_str, "%"PRIu32"x%"PRIu32, new_cx, new_cy);

		obs_property_list_add_string(p, scale_str, scale_str);
	}

	obs_property_set_enabled(p, false);

	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY,
			TEXT_ALLOW_TRANSPARENCY);

	obs_properties_add_bool(ppts, SETTING_LIMIT_FRAMERATE,
			TEXT_LIMIT_FRAMERATE);

	obs_properties_add_bool(ppts, SETTING_CAPTURE_OVERLAYS,
			TEXT_CAPTURE_OVERLAYS);

	UNUSED_PARAMETER(data);
	return ppts;
}


struct obs_source_info game_capture_info = {
	.id = "game_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = game_capture_name,
	.create = game_capture_create,
	.destroy = game_capture_destroy,
	.get_width = game_capture_width,
	.get_height = game_capture_height,
	.get_defaults = game_capture_defaults,
	.get_properties = game_capture_properties,
	.update = game_capture_update,
	.video_tick = game_capture_tick,
	.video_render = game_capture_render
};
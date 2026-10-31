use std::env;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};

unsafe extern "C" {
    // The Vita host owns the SDL_Window/SDL_Renderer, so it uses the 4-arg entry.
    // Plain `external_main` is the 2-arg main-shaped symbol SDL's Android glue calls.
    fn external_main_ex(argc: c_int, argv: *const *const c_char, external_window: *mut c_void, external_renderer: *mut c_void) -> c_int;
    fn SDL_Init(flags: u32) -> c_int;
    #[cfg(not(vita_target))]
    fn SDL_Quit();
    fn SDL_CreateWindow(
        title: *const c_char,
        x: c_int,
        y: c_int,
        w: c_int,
        h: c_int,
        flags: u32,
    ) -> *mut c_void;
    fn SDL_CreateRenderer(window: *mut c_void, index: c_int, flags: u32) -> *mut c_void;
    #[cfg(not(vita_target))]
    fn SDL_DestroyRenderer(renderer: *mut c_void);
    #[cfg(not(vita_target))]
    fn SDL_DestroyWindow(window: *mut c_void);
    fn SDL_GetError() -> *const c_char;
}

fn main() -> Result<(), String> {
    const SDL_INIT_AUDIO: u32 = 0x0000_0010;
    const SDL_INIT_VIDEO: u32 = 0x0000_0020;
    const SDL_INIT_GAMECONTROLLER: u32 = 0x0000_2000;
    const SDL_INIT_EVENTS: u32 = 0x0000_4000;
    const SDL_WINDOWPOS_CENTERED: i32 = 0x2FFF_0000u32 as i32;
    const SDL_RENDERER_ACCELERATED: u32 = 0x0000_0002;
    const SDL_RENDERER_PRESENTVSYNC: u32 = 0x0000_0004;

    let _ = env::set_current_dir("app0:/");
    unsafe {
        if SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0 {
            return Err(sdl_error("SDL_Init"));
        }
    }

    let title = CString::new("ARMSX PSVita Host").map_err(|err| err.to_string())?;
    let width = 960;
    let height = 544;
    let raw_window = unsafe {
        SDL_CreateWindow(
            title.as_ptr(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width,
            height,
            0,
        )
    };
    if raw_window.is_null() {
        unsafe { cleanup_sdl(std::ptr::null_mut(), std::ptr::null_mut()) };
        return Err(sdl_error("SDL_CreateWindow"));
    }

    let raw_renderer =
        unsafe { SDL_CreateRenderer(raw_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) };
    if raw_renderer.is_null() {
        unsafe { cleanup_sdl(raw_window, std::ptr::null_mut()) };
        return Err(sdl_error("SDL_CreateRenderer"));
    }

    let mut args = env::args().collect::<Vec<_>>();
    if args.is_empty() {
        args.push(String::from("armsx"));
    }
    let cstrings: Vec<CString> = args
        .iter()
        .map(|s| CString::new(s.as_str()).map_err(|err| err.to_string()))
        .collect::<Result<_, _>>()?;
    let mut ptrs: Vec<*const c_char> = cstrings.iter().map(|s| s.as_ptr()).collect();
    ptrs.push(std::ptr::null());

    let ret = unsafe { external_main_ex((ptrs.len() - 1) as c_int, ptrs.as_ptr(), raw_window, raw_renderer) };

    unsafe {
        cleanup_sdl(raw_window, raw_renderer);
    }

    if ret != 0 {
        return Err(format!("external_main_ex returned non-zero: {}", ret));
    }

    Ok(())
}

fn sdl_error(prefix: &str) -> String {
    let message = unsafe {
        let raw = SDL_GetError();
        if raw.is_null() {
            String::from("unknown SDL error")
        } else {
            CStr::from_ptr(raw).to_string_lossy().into_owned()
        }
    };

    format!("{prefix} failed: {message}")
}

#[cfg(vita_target)]
unsafe fn cleanup_sdl(_window: *mut c_void, _renderer: *mut c_void) {}

#[cfg(not(vita_target))]
unsafe fn cleanup_sdl(window: *mut c_void, renderer: *mut c_void) {
    if !renderer.is_null() {
        SDL_DestroyRenderer(renderer);
    }
    if !window.is_null() {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

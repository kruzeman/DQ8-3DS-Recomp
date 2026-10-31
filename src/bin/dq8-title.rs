//! Native SDL runner for the first DQ8 title-screen rendering milestone.
//!
//! It decodes the game's BFLIM resources directly and presents the two 3DS
//! screens in one desktop window. This is the first runtime slice; it does not
//! yet execute the game's ARM title/menu code or BFLAN animation tracks.

use std::{env, ffi::CString, fs, path::Path, ptr, time::Duration};

#[link(name = "SDL2")]
unsafe extern "C" {
    fn SDL_Init(flags: u32) -> i32;
    fn SDL_Quit();
    fn SDL_CreateWindow(
        title: *const i8,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        flags: u32,
    ) -> *mut SdlWindow;
    fn SDL_CreateRenderer(window: *mut SdlWindow, index: i32, flags: u32) -> *mut SdlRenderer;
    fn SDL_CreateTexture(
        renderer: *mut SdlRenderer,
        format: u32,
        access: i32,
        w: i32,
        h: i32,
    ) -> *mut SdlTexture;
    fn SDL_UpdateTexture(
        texture: *mut SdlTexture,
        rect: *const SdlRect,
        pixels: *const u8,
        pitch: i32,
    ) -> i32;
    fn SDL_RenderClear(renderer: *mut SdlRenderer) -> i32;
    fn SDL_RenderCopy(
        renderer: *mut SdlRenderer,
        texture: *mut SdlTexture,
        src: *const SdlRect,
        dst: *const SdlRect,
    ) -> i32;
    fn SDL_RenderPresent(renderer: *mut SdlRenderer);
    fn SDL_PollEvent(event: *mut SdlEvent) -> i32;
    fn SDL_DestroyTexture(texture: *mut SdlTexture);
    fn SDL_DestroyRenderer(renderer: *mut SdlRenderer);
    fn SDL_DestroyWindow(window: *mut SdlWindow);
    fn SDL_GetError() -> *const i8;
}

#[repr(C)]
struct SdlWindow {
    _private: [u8; 0],
}
#[repr(C)]
struct SdlRenderer {
    _private: [u8; 0],
}
#[repr(C)]
struct SdlTexture {
    _private: [u8; 0],
}
#[repr(C)]
struct SdlRect {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}
#[repr(C, align(8))]
struct SdlEvent {
    bytes: [u8; 56],
}

const SDL_INIT_VIDEO: u32 = 0x20;
const SDL_WINDOW_SHOWN: u32 = 0x04;
const SDL_RENDERER_ACCELERATED: u32 = 0x02;
const SDL_RENDERER_PRESENTVSYNC: u32 = 0x04;
const SDL_PIXELFORMAT_RGBA32: u32 = 0x1676_2004; // SDL_PIXELFORMAT_ABGR8888 on little-endian hosts

#[derive(Clone)]
struct Image {
    w: usize,
    h: usize,
    px: Vec<u8>,
}

impl Image {
    fn blank(w: usize, h: usize, color: [u8; 4]) -> Self {
        let mut px = vec![0; w * h * 4];
        for p in px.chunks_exact_mut(4) {
            p.copy_from_slice(&color);
        }
        Self { w, h, px }
    }
    fn put(&mut self, x: usize, y: usize, c: [u8; 4]) {
        if x < self.w && y < self.h {
            self.px[(y * self.w + x) * 4..][..4].copy_from_slice(&c);
        }
    }
    fn blend(&mut self, x: usize, y: usize, c: [u8; 4]) {
        if x >= self.w || y >= self.h {
            return;
        }
        let i = (y * self.w + x) * 4;
        let a = c[3] as u32;
        if a == 255 {
            self.px[i..i + 4].copy_from_slice(&c);
            return;
        }
        if a == 0 {
            return;
        }
        let inv = 255 - a;
        for k in 0..3 {
            self.px[i + k] = ((c[k] as u32 * a + self.px[i + k] as u32 * inv) / 255) as u8;
        }
        self.px[i + 3] = (a + self.px[i + 3] as u32 * inv / 255) as u8;
    }
    fn draw(&mut self, src: &Image, x: i32, y: i32, w: usize, h: usize) {
        if w == 0 || h == 0 {
            return;
        }
        for dy in 0..h {
            let sy = dy * src.h / h;
            let py = y + dy as i32;
            if py < 0 || py >= self.h as i32 {
                continue;
            }
            for dx in 0..w {
                let px = x + dx as i32;
                if px < 0 || px >= self.w as i32 {
                    continue;
                }
                let sx = dx * src.w / w;
                let si = (sy * src.w + sx) * 4;
                self.blend(
                    px as usize,
                    py as usize,
                    src.px[si..si + 4].try_into().unwrap(),
                );
            }
        }
    }
}

fn u16le(b: &[u8], i: usize) -> Result<u16, String> {
    Ok(u16::from_le_bytes(
        b.get(i..i + 2).ok_or("truncated u16")?.try_into().unwrap(),
    ))
}
fn u32le(b: &[u8], i: usize) -> Result<u32, String> {
    Ok(u32::from_le_bytes(
        b.get(i..i + 4).ok_or("truncated u32")?.try_into().unwrap(),
    ))
}
fn expand5(v: u32) -> u8 {
    ((v << 3) | (v >> 2)) as u8
}
fn expand4(v: u32) -> u8 {
    ((v << 4) | v) as u8
}
fn sign3(v: u32) -> i32 {
    if v & 4 != 0 { v as i32 - 8 } else { v as i32 }
}

const MOD: [[i32; 4]; 8] = [
    [2, 8, -2, -8],
    [5, 17, -5, -17],
    [9, 29, -9, -29],
    [13, 42, -13, -42],
    [18, 60, -18, -60],
    [24, 80, -24, -80],
    [33, 106, -33, -106],
    [47, 183, -47, -183],
];

fn etc_block(bytes: &[u8]) -> Result<[[u8; 3]; 16], String> {
    if bytes.len() < 8 {
        return Err("truncated ETC1 block".into());
    }
    let bits = u64::from_le_bytes(bytes[..8].try_into().unwrap());
    let hi = (bits >> 32) as u32;
    let lo = bits as u32;
    let diff = (hi >> 1) & 1 != 0;
    let flip = hi & 1 != 0;
    let mut c = [[0i32; 3]; 2];
    if diff {
        let r = (hi >> 27) & 31;
        let g = (hi >> 19) & 31;
        let b = (hi >> 11) & 31;
        let r2 = (r as i32 + sign3((hi >> 24) & 7)) & 31;
        let g2 = (g as i32 + sign3((hi >> 16) & 7)) & 31;
        let b2 = (b as i32 + sign3((hi >> 8) & 7)) & 31;
        c[0] = [expand5(r) as i32, expand5(g) as i32, expand5(b) as i32];
        c[1] = [
            expand5(r2 as u32) as i32,
            expand5(g2 as u32) as i32,
            expand5(b2 as u32) as i32,
        ];
    } else {
        c[0] = [
            expand4((hi >> 28) & 15) as i32,
            expand4((hi >> 20) & 15) as i32,
            expand4((hi >> 12) & 15) as i32,
        ];
        c[1] = [
            expand4((hi >> 24) & 15) as i32,
            expand4((hi >> 16) & 15) as i32,
            expand4((hi >> 8) & 15) as i32,
        ];
    }
    let tables = [((hi >> 5) & 7) as usize, ((hi >> 2) & 7) as usize];
    let mut out = [[0; 3]; 16];
    for y in 0..4 {
        for x in 0..4 {
            let sub = if flip {
                usize::from(y >= 2)
            } else {
                usize::from(x >= 2)
            };
            let n = x * 4 + y;
            let selector = (((lo >> (n + 16)) & 1) * 2 + ((lo >> n) & 1)) as usize;
            let m = MOD[tables[sub]][selector];
            for k in 0..3 {
                out[y * 4 + x][k] = (c[sub][k] + m).clamp(0, 255) as u8;
            }
        }
    }
    Ok(out)
}

fn decode_bflim(path: &Path) -> Result<Image, String> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let f = data.len().checked_sub(40).ok_or("BFLIM too short")?;
    if &data[f..f + 4] != b"FLIM" || &data[f + 20..f + 24] != b"imag" {
        return Err(format!("{} is not BFLIM", path.display()));
    }
    if data[f + 4..f + 6] != [0xff, 0xfe] {
        return Err("only little-endian BFLIM is supported".into());
    }
    let w = u16le(&data, f + 28)? as usize;
    let h = u16le(&data, f + 30)? as usize;
    let fmt = data[f + 34];
    let raw_len = u32le(&data, f + 36)? as usize;
    if w == 0 || h == 0 || raw_len > f {
        return Err("invalid BFLIM dimensions/data length".into());
    }
    let raw = &data[..raw_len];
    let mut out = Image::blank(w, h, [0, 0, 0, 0]);
    if fmt == 10 || fmt == 11 {
        let bpp = if fmt == 10 { 8 } else { 16 };
        let bx = w.div_ceil(4);
        let by = h.div_ceil(4);
        let tx = bx.div_ceil(2);
        let ty = by.div_ceil(2);
        let needed = tx * ty * 4 * bpp;
        if raw.len() < needed {
            return Err("truncated ETC1 texture".into());
        }
        for tile_y in 0..ty {
            for tile_x in 0..tx {
                for block_y in 0..2 {
                    for block_x in 0..2 {
                        let x0 = tile_x * 8 + block_x * 4;
                        let y0 = tile_y * 8 + block_y * 4;
                        if x0 >= w || y0 >= h {
                            continue;
                        }
                        let block_i = ((tile_y * tx + tile_x) * 4 + block_y * 2 + block_x) * bpp;
                        let a = if fmt == 11 {
                            &raw[block_i..block_i + 8]
                        } else {
                            &[0xff; 8]
                        };
                        let color_start = if fmt == 11 { block_i + 8 } else { block_i };
                        let colors = etc_block(&raw[color_start..color_start + 8])?;
                        for py in 0..4 {
                            for px in 0..4 {
                                if x0 + px >= w || y0 + py >= h {
                                    continue;
                                }
                                let ai = px * 4 + py;
                                let ab = a[ai / 2];
                                let alpha = if fmt == 11 {
                                    ((ab >> ((ai & 1) * 4)) & 15) * 17
                                } else {
                                    255
                                };
                                let c = colors[py * 4 + px];
                                out.put(x0 + px, y0 + py, [c[0], c[1], c[2], alpha]);
                            }
                        }
                    }
                }
            }
        }
    } else if fmt == 0 || fmt == 1 || fmt == 5 {
        let bpp = if fmt == 5 { 2 } else { 1 };
        let tiles_x = w.div_ceil(8);
        let tiles_y = h.div_ceil(8);
        if raw.len() < tiles_x * tiles_y * 64 * bpp {
            return Err("truncated tiled texture".into());
        }
        let mut off = 0;
        for ty in 0..tiles_y {
            for tx in 0..tiles_x {
                for qy in 0..2 {
                    for qx in 0..2 {
                        for gy in 0..2 {
                            for gx in 0..2 {
                                for py in 0..2 {
                                    for px in 0..2 {
                                        let x = tx * 8 + qx * 4 + gx * 2 + px;
                                        let y = ty * 8 + qy * 4 + gy * 2 + py;
                                        if x < w && y < h {
                                            if fmt == 5 {
                                                let v = u16::from_le_bytes([raw[off], raw[off + 1]])
                                                    as u32;
                                                out.put(
                                                    x,
                                                    y,
                                                    [
                                                        expand5((v >> 11) & 31),
                                                        (((v >> 5) & 63) * 255 / 63) as u8,
                                                        expand5(v & 31),
                                                        255,
                                                    ],
                                                );
                                            } else if fmt == 1 {
                                                out.put(x, y, [255, 255, 255, raw[off]]);
                                            } else {
                                                let c = raw[off];
                                                out.put(x, y, [c, c, c, 255]);
                                            }
                                        }
                                        off += bpp;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
        return Err(format!("unsupported BFLIM format {fmt}"));
    }
    Ok(out)
}

fn load(assets: &Path, name: &str) -> Result<Image, String> {
    decode_bflim(&assets.join("timg").join(name))
}

fn animated_alpha(path: &Path, node: &str, frame: f32) -> Result<Option<u8>, String> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if data.get(..4) != Some(b"FLAN") || data.get(4..6) != Some(&[0xff, 0xfe]) {
        return Err(format!("{} is not a little-endian BFLAN", path.display()));
    }
    let count = u16le(&data, 0x10)? as usize;
    let mut section = 0x14;
    for _ in 0..count {
        let tag = data
            .get(section..section + 4)
            .ok_or("truncated BFLAN section")?;
        let size = u32le(&data, section + 4)? as usize;
        if size < 8 || section + size > data.len() {
            return Err("invalid BFLAN section size".into());
        }
        if tag == b"pai1" {
            let entries = u16le(&data, section + 14)? as usize;
            let table = u32le(&data, section + 16)? as usize;
            for i in 0..entries {
                let entry = section + u32le(&data, section + table + i * 4)? as usize;
                let name_end = data
                    .get(entry..entry + 28)
                    .ok_or("truncated BFLAN entry")?
                    .iter()
                    .position(|b| *b == 0)
                    .unwrap_or(28);
                if &data[entry..entry + name_end] != node.as_bytes() {
                    continue;
                }
                let tags = data[entry + 28] as usize;
                for j in 0..tags {
                    let t = entry + u32le(&data, entry + 32 + j * 4)? as usize;
                    if data.get(t..t + 4) != Some(b"FLVC") {
                        continue;
                    }
                    let tracks = u32le(&data, t + 4)? as usize;
                    for k in 0..tracks {
                        let a = t + u32le(&data, t + 8 + k * 4)? as usize;
                        let target = *data.get(a + 1).ok_or("truncated BFLAN track")?;
                        let data_type = u16le(&data, a + 2)?;
                        let keys = u16le(&data, a + 4)? as usize;
                        let key_data = u32le(&data, a + 8)? as usize;
                        if target != 16 || data_type != 2 || keys == 0 {
                            continue;
                        }
                        let mut prior = None;
                        for n in 0..keys {
                            let p = a + key_data + n * 12;
                            let key_frame = f32::from_bits(u32le(&data, p)?);
                            let value = f32::from_bits(u32le(&data, p + 4)?);
                            if key_frame <= frame {
                                prior = Some(value);
                            } else {
                                break;
                            }
                        }
                        if let Some(alpha) = prior {
                            return Ok(Some(alpha.round().clamp(0.0, 255.0) as u8));
                        }
                    }
                }
            }
        }
        section += size;
    }
    Ok(None)
}

fn set_alpha(image: &mut Image, alpha: u8) {
    for p in image.px.chunks_exact_mut(4) {
        p[3] = ((p[3] as u16 * alpha as u16) / 255) as u8;
    }
}

fn transpose(src: &Image) -> Image {
    let mut out = Image::blank(src.h, src.w, [0, 0, 0, 0]);
    for y in 0..src.h {
        for x in 0..src.w {
            let i = (y * src.w + x) * 4;
            out.put(y, x, src.px[i..i + 4].try_into().unwrap());
        }
    }
    out
}

fn compose(upper_dir: &Path, lower_dir: &Path) -> Result<Image, String> {
    let mut screen = Image::blank(400, 480, [12, 10, 8, 255]);
    let map = load(upper_dir, "title_wmap.bflim")?;
    screen.draw(&map, 0, 0, 400, 240);
    let logo_alpha = animated_alpha(
        &upper_dir.join("anim/title001_u_in.bflan"),
        "title_logo_j",
        30.0,
    )?
    .unwrap_or(0);
    // The English logo is a two-eye atlas. The layout material samples one
    // eye and applies the atlas's 90-degree coordinate transform.
    let atlas = load(upper_dir, "title_logo_en.bflim")?;
    let mut eye = Image::blank(atlas.w / 2, atlas.h, [0, 0, 0, 0]);
    for y in 0..atlas.h {
        for x in 0..atlas.w / 2 {
            let i = (y * atlas.w + x) * 4;
            eye.put(x, y, atlas.px[i..i + 4].try_into().unwrap());
        }
    }
    let logo = transpose(&eye);
    let mut logo = logo;
    set_alpha(&mut logo, logo_alpha);
    screen.draw(&logo, 78, 4, 244, 244);
    let lower_alpha = animated_alpha(
        &lower_dir.join("anim/title000_l_wait.bflan"),
        "3DS320_240_Lower",
        0.0,
    )?
    .unwrap_or(0);
    // In the title-in state, the lower picture's BFLAN track keeps it hidden.
    // Leave that screen black until the layout's full animation path is wired.
    let _ = lower_alpha;
    Ok(screen)
}

fn write_bmp(path: &Path, image: &Image) -> Result<(), String> {
    let row_bytes = image.w.checked_mul(3).ok_or("BMP row overflow")?;
    let pixel_bytes = row_bytes.checked_mul(image.h).ok_or("BMP size overflow")?;
    let file_bytes = 54usize
        .checked_add(pixel_bytes)
        .ok_or("BMP size overflow")?;
    let mut bytes = vec![0u8; file_bytes];
    bytes[..2].copy_from_slice(b"BM");
    bytes[2..6].copy_from_slice(&(file_bytes as u32).to_le_bytes());
    bytes[10..14].copy_from_slice(&54u32.to_le_bytes());
    bytes[14..18].copy_from_slice(&40u32.to_le_bytes());
    bytes[18..22].copy_from_slice(&(image.w as i32).to_le_bytes());
    bytes[22..26].copy_from_slice(&(image.h as i32).to_le_bytes());
    bytes[26..28].copy_from_slice(&1u16.to_le_bytes());
    bytes[28..30].copy_from_slice(&24u16.to_le_bytes());
    bytes[34..38].copy_from_slice(&(pixel_bytes as u32).to_le_bytes());
    for y in 0..image.h {
        let dst_y = image.h - 1 - y;
        for x in 0..image.w {
            let s = (y * image.w + x) * 4;
            let d = 54 + dst_y * row_bytes + x * 3;
            bytes[d..d + 3].copy_from_slice(&[image.px[s + 2], image.px[s + 1], image.px[s]]);
        }
    }
    fs::write(path, bytes).map_err(|e| format!("{}: {e}", path.display()))
}

fn sdl_error(what: &str) -> String {
    // SAFETY: SDL_GetError returns a NUL-terminated string owned by SDL.
    let msg = unsafe { std::ffi::CStr::from_ptr(SDL_GetError()) }.to_string_lossy();
    format!("{what}: {msg}")
}

fn run(image: &Image) -> Result<(), String> {
    // SAFETY: SDL objects are created, used, and destroyed on this thread.
    unsafe {
        if SDL_Init(SDL_INIT_VIDEO) != 0 {
            return Err(sdl_error("SDL_Init"));
        }
        let title = CString::new("DQ8 static recomp — title screen").unwrap();
        let window = SDL_CreateWindow(
            title.as_ptr(),
            0x2fff0000u32 as i32,
            0x2fff0000u32 as i32,
            800,
            960,
            SDL_WINDOW_SHOWN,
        );
        if window.is_null() {
            SDL_Quit();
            return Err(sdl_error("SDL_CreateWindow"));
        }
        let renderer = SDL_CreateRenderer(
            window,
            -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
        );
        if renderer.is_null() {
            SDL_DestroyWindow(window);
            SDL_Quit();
            return Err(sdl_error("SDL_CreateRenderer"));
        }
        let texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            0,
            image.w as i32,
            image.h as i32,
        );
        if texture.is_null() {
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return Err(sdl_error("SDL_CreateTexture"));
        }
        if SDL_UpdateTexture(
            texture,
            ptr::null(),
            image.px.as_ptr(),
            (image.w * 4) as i32,
        ) != 0
        {
            return Err(sdl_error("SDL_UpdateTexture"));
        }
        let dst = SdlRect {
            x: 0,
            y: 0,
            w: 800,
            h: 960,
        };
        let mut quit = false;
        let mut event = SdlEvent { bytes: [0; 56] };
        while !quit {
            while SDL_PollEvent(&mut event) != 0 {
                let kind = u32::from_ne_bytes(event.bytes[..4].try_into().unwrap());
                if kind == 0x100 {
                    quit = true;
                }
                if kind == 0x300 {
                    let key = u32::from_ne_bytes(event.bytes[20..24].try_into().unwrap());
                    if key == 27 || key == b'q' as u32 {
                        quit = true;
                    }
                }
            }
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, ptr::null(), &dst);
            SDL_RenderPresent(renderer);
            std::thread::sleep(Duration::from_millis(10));
        }
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    Ok(())
}

fn main() {
    let args: Vec<_> = env::args_os().collect();
    if args.len() == 5 && args[1] == "--snapshot" {
        if let Err(e) = compose(Path::new(&args[3]), Path::new(&args[4]))
            .and_then(|img| write_bmp(Path::new(&args[2]), &img))
        {
            eprintln!("dq8-title: {e}");
            std::process::exit(1);
        }
        return;
    }
    if args.len() != 3 {
        eprintln!(
            "Usage: dq8-title <extracted-title_u-dir> <extracted-title_l-dir>\n       dq8-title --snapshot OUTPUT.bmp <extracted-title_u-dir> <extracted-title_l-dir>\nExtracted directories must each contain blyt/ and timg/."
        );
        std::process::exit(2);
    }
    match compose(Path::new(&args[1]), Path::new(&args[2])).and_then(|img| run(&img)) {
        Ok(()) => {}
        Err(e) => {
            eprintln!("dq8-title: {e}");
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bflim_l8_tile(data: [u8; 64]) -> Vec<u8> {
        let mut file = vec![0u8; 64 + 40];
        file[..64].copy_from_slice(&data);
        let f = 64;
        file[f..f + 4].copy_from_slice(b"FLIM");
        file[f + 4..f + 6].copy_from_slice(&[0xff, 0xfe]);
        file[f + 20..f + 24].copy_from_slice(b"imag");
        file[f + 28..f + 30].copy_from_slice(&8u16.to_le_bytes());
        file[f + 30..f + 32].copy_from_slice(&8u16.to_le_bytes());
        file[f + 34] = 0; // L8
        file[f + 35] = 4; // 8x8 tiled
        file[f + 36..f + 40].copy_from_slice(&64u32.to_le_bytes());
        file
    }

    #[test]
    fn tiled_l8_texels_are_unswizzled_into_rows() {
        let mut bytes = [0; 64];
        for (n, byte) in bytes.iter_mut().enumerate() {
            *byte = n as u8;
        }
        let path = std::env::temp_dir().join(format!("dq8-title-l8-{}.bflim", std::process::id()));
        fs::write(&path, bflim_l8_tile(bytes)).unwrap();
        let image = decode_bflim(&path).unwrap();
        let _ = fs::remove_file(path);
        let row: Vec<u8> = (0..8).map(|x| image.px[x * 4]).collect();
        assert_eq!(row, [0, 1, 4, 5, 16, 17, 20, 21]);
        assert_eq!(image.px[(8 + 0) * 4], 2);
        assert_eq!(image.px[(8 + 2) * 4], 6);
    }
}

#![no_main]
#![no_std]

use core::mem::size_of;
use core::panic::PanicInfo;
use core::ptr;
use core::slice;

include!("../../../sdk/rust/webos.rs");

webos_declare_abi_version!();

const DEFAULT_URL: &[u8] = b"https://api.api-ninjas.com/v1/sudokugenerate?difficulty=easy";

fn log(message: &[u8]) {
    unsafe { webos_log(WEBOS_LOG_INFO, message.as_ptr(), message.len() as u32) }
}

unsafe fn argument(argv: *const *const u8, index: usize) -> Option<&'static [u8]> {
    if argv.is_null() {
        return None;
    }

    let value = unsafe { *argv.add(index) };
    if value.is_null() {
        return None;
    }

    let mut length = 0;
    while unsafe { *value.add(length) } != 0 {
        length += 1;
    }
    Some(unsafe { slice::from_raw_parts(value, length) })
}

struct Buffer<'a> {
    bytes: &'a mut [u8],
    used: usize,
}

impl Buffer<'_> {
    fn push(&mut self, value: &[u8]) -> bool {
        let Some(end) = self.used.checked_add(value.len()) else {
            return false;
        };
        if end > self.bytes.len() {
            return false;
        }
        self.bytes[self.used..end].copy_from_slice(value);
        self.used = end;
        true
    }

    fn push_u32(&mut self, mut value: u32) {
        let mut digits = [0_u8; 10];
        let mut count = 0;
        loop {
            digits[count] = b'0' + (value % 10) as u8;
            count += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        while count > 0 {
            count -= 1;
            if !self.push(&digits[count..count + 1]) {
                break;
            }
        }
    }

    fn as_slice(&self) -> &[u8] {
        &self.bytes[..self.used]
    }
}

/// # Safety
///
/// The WebOS runtime must provide `argc` valid NUL-terminated strings through `argv`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn main(argc: i32, argv: *const *const u8) -> i32 {
    if argc < 2 {
        log(b"usage: rust_sudoku <api-key> [url]");
        return 1;
    }

    let api_key = unsafe { argument(argv, 1) }.unwrap_or(b"");
    let url = if argc > 2 {
        unsafe { argument(argv, 2) }.unwrap_or(DEFAULT_URL)
    } else {
        DEFAULT_URL
    };

    let mut header_storage = [0_u8; 256];
    let mut headers = Buffer {
        bytes: &mut header_storage,
        used: 0,
    };
    if !headers.push(b"X-Api-Key: ")
        || !headers.push(api_key)
        || !headers.push(b"\r\nAccept: application/json\r\n")
    {
        log(b"rust_sudoku: API key is too long");
        return 1;
    }

    let mut body = [0_u8; 2048];
    let mut response = WebHttpResponse {
        struct_size: size_of::<WebHttpResponse>() as u32,
        status_code: 0,
        body_len: 0,
        content_length: 0,
        flags: 0,
    };

    log(b"rust_sudoku: requesting puzzle");
    let result = unsafe {
        web_http_request(
            WEB_HTTP_GET,
            url.as_ptr(),
            url.len() as u32,
            headers.as_slice().as_ptr(),
            headers.as_slice().len() as u32,
            ptr::null(),
            0,
            body.as_mut_ptr(),
            body.len() as u32,
            &mut response,
            size_of::<WebHttpResponse>() as u32,
            15_000,
        )
    };

    if result != WEB_HTTP_OK {
        let mut status_storage = [0_u8; 64];
        let mut status = Buffer {
            bytes: &mut status_storage,
            used: 0,
        };
        status.push(b"rust_sudoku: request error ");
        if result < 0 {
            status.push(b"-");
            status.push_u32(result.unsigned_abs());
        } else {
            status.push_u32(result as u32);
        }
        log(status.as_slice());
        return 1;
    }

    let mut status_storage = [0_u8; 32];
    let mut status = Buffer {
        bytes: &mut status_storage,
        used: 0,
    };
    status.push(b"rust_sudoku: HTTP ");
    status.push_u32(response.status_code);
    log(status.as_slice());

    let body_len = (response.body_len as usize).min(body.len());
    log(&body[..body_len]);
    if (200..300).contains(&response.status_code) {
        0
    } else {
        1
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

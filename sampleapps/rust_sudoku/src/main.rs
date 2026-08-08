#![cfg_attr(not(test), no_main)]
#![no_std]

use core::mem::size_of;
use core::panic::PanicInfo;
use core::ptr;
use core::slice;

include!("../../../sdk/rust/webos.rs");

#[cfg(not(test))]
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

fn skip_whitespace(input: &[u8], cursor: &mut usize) {
    while *cursor < input.len() && input[*cursor].is_ascii_whitespace() {
        *cursor += 1;
    }
}

fn consume(input: &[u8], cursor: &mut usize, expected: u8) -> bool {
    skip_whitespace(input, cursor);
    if input.get(*cursor) != Some(&expected) {
        return false;
    }
    *cursor += 1;
    true
}

fn find_key(input: &[u8], key: &[u8]) -> Option<usize> {
    input
        .windows(key.len())
        .position(|window| window == key)
        .map(|position| position + key.len())
}

fn parse_cell(input: &[u8], cursor: &mut usize) -> Option<u8> {
    skip_whitespace(input, cursor);
    if input.get(*cursor..)?.starts_with(b"null") {
        *cursor += 4;
        return Some(0);
    }

    let value = *input.get(*cursor)?;
    if !(b'1'..=b'9').contains(&value) {
        return None;
    }
    *cursor += 1;
    Some(value - b'0')
}

fn parse_puzzle(input: &[u8]) -> Option<[u8; 81]> {
    let mut cursor = find_key(input, b"\"puzzle\"")?;
    if !consume(input, &mut cursor, b':') || !consume(input, &mut cursor, b'[') {
        return None;
    }

    let mut grid = [0_u8; 81];
    for row in 0..9 {
        if !consume(input, &mut cursor, b'[') {
            return None;
        }
        for column in 0..9 {
            grid[row * 9 + column] = parse_cell(input, &mut cursor)?;
            if column < 8 && !consume(input, &mut cursor, b',') {
                return None;
            }
        }
        if !consume(input, &mut cursor, b']') || (row < 8 && !consume(input, &mut cursor, b',')) {
            return None;
        }
    }
    consume(input, &mut cursor, b']').then_some(grid)
}

fn can_place(grid: &[u8; 81], index: usize, value: u8) -> bool {
    let row = index / 9;
    let column = index % 9;
    for offset in 0..9 {
        let row_index = row * 9 + offset;
        let column_index = offset * 9 + column;
        if (row_index != index && grid[row_index] == value)
            || (column_index != index && grid[column_index] == value)
        {
            return false;
        }
    }

    let box_row = (row / 3) * 3;
    let box_column = (column / 3) * 3;
    for row_offset in 0..3 {
        for column_offset in 0..3 {
            let box_index = (box_row + row_offset) * 9 + box_column + column_offset;
            if box_index != index && grid[box_index] == value {
                return false;
            }
        }
    }
    true
}

fn solve(grid: &mut [u8; 81]) -> bool {
    let givens = *grid;
    for (index, &value) in givens.iter().enumerate() {
        if value > 9 || (value != 0 && !can_place(&givens, index, value)) {
            return false;
        }
    }

    let mut next_candidate = [1_u8; 81];
    let mut index = 0_usize;
    while index < grid.len() {
        if givens[index] != 0 {
            index += 1;
            continue;
        }

        grid[index] = 0;
        let mut candidate = next_candidate[index];
        while candidate <= 9 && !can_place(grid, index, candidate) {
            candidate += 1;
        }
        if candidate <= 9 {
            grid[index] = candidate;
            next_candidate[index] = candidate + 1;
            index += 1;
            continue;
        }

        next_candidate[index] = 1;
        loop {
            if index == 0 {
                return false;
            }
            index -= 1;
            if givens[index] == 0 {
                break;
            }
        }
    }
    true
}

fn log_grid(label: &[u8], grid: &[u8; 81]) {
    log(label);
    for row in grid.chunks_exact(9) {
        let mut line = [b' '; 17];
        for (column, &value) in row.iter().enumerate() {
            line[column * 2] = if value == 0 { b'.' } else { b'0' + value };
        }
        log(&line);
    }
}

/// # Safety
///
/// The WebOS runtime must provide `argc` valid NUL-terminated strings through `argv`.
#[cfg(not(test))]
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

    if !(200..300).contains(&response.status_code) {
        return 1;
    }

    let body_len = (response.body_len as usize).min(body.len());
    let Some(mut puzzle) = parse_puzzle(&body[..body_len]) else {
        log(b"rust_sudoku: invalid puzzle response");
        return 1;
    };

    log_grid(b"rust_sudoku: puzzle", &puzzle);
    if !solve(&mut puzzle) {
        log(b"rust_sudoku: puzzle has no solution");
        return 1;
    }
    log_grid(b"rust_sudoku: solution", &puzzle);
    0
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[cfg(test)]
mod tests {
    use super::*;

    const RESPONSE: &[u8] = br#"{"puzzle": [[2, 4, 7, 9, 3, 6, null, null, 8], [6, 5, 1, 2, 7, 8, 3, null, 9], [null, 9, null, 1, null, 5, 2, null, 7], [1, 3, 5, null, null, 4, null, 7, 2], [7, 2, 6, 8, 1, 3, null, 9, null], [9, 8, null, null, 5, 2, 6, 3, 1], [3, null, 2, null, null, 1, 9, null, 4], [4, 1, 8, null, 2, null, 7, 5, 6], [5, 6, 9, 4, 8, 7, 1, 2, 3]]}"#;

    #[test]
    fn parses_and_solves_api_response() {
        let mut puzzle = parse_puzzle(RESPONSE).expect("puzzle should parse");
        assert!(solve(&mut puzzle));
        assert_eq!(
            puzzle,
            [
                2, 4, 7, 9, 3, 6, 5, 1, 8, 6, 5, 1, 2, 7, 8, 3, 4, 9, 8, 9, 3, 1, 4, 5, 2, 6, 7, 1,
                3, 5, 6, 9, 4, 8, 7, 2, 7, 2, 6, 8, 1, 3, 4, 9, 5, 9, 8, 4, 7, 5, 2, 6, 3, 1, 3, 7,
                2, 5, 6, 1, 9, 8, 4, 4, 1, 8, 3, 2, 9, 7, 5, 6, 5, 6, 9, 4, 8, 7, 1, 2, 3,
            ]
        );
    }

    #[test]
    fn rejects_invalid_and_unsolvable_puzzles() {
        let mut invalid = [0_u8; 81];
        invalid[0] = 1;
        invalid[1] = 1;
        assert!(!solve(&mut invalid));

        let mut unsolvable = parse_puzzle(RESPONSE).expect("puzzle should parse");
        unsolvable[25] = 4;
        assert!(!solve(&mut unsolvable));
        assert!(parse_puzzle(br#"{"solution": []}"#).is_none());
    }
}

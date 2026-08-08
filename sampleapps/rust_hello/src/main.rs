#![no_main]
#![no_std]

use core::panic::PanicInfo;

unsafe extern "C" {
    fn log_print(message: *const u8);
}

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    unsafe {
        log_print(c"hello from rust".as_ptr().cast());
    }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

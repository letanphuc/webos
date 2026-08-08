#![no_main]
#![no_std]
use core::panic::PanicInfo;
include!("../../../sdk/rust/webos.rs");

webos_declare_abi_version!();

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    let message = b"hello from rust";
    unsafe { webos_log(WEBOS_LOG_INFO, message.as_ptr(), message.len() as u32); }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! { loop {} }

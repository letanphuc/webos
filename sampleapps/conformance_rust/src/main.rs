#![no_main]
#![no_std]
use core::panic::PanicInfo;
include!("../../../sdk/rust/webos.rs");
webos_declare_abi_version!();
#[unsafe(no_mangle)] pub extern "C" fn main() -> i32 {
    let message = b"WebOS ABI v1 Rust conformance";
    unsafe { webos_log(WEBOS_LOG_INFO, message.as_ptr(), message.len() as u32); webos_ready(); webos_heartbeat(); }
    0
}
#[panic_handler] fn panic(_: &PanicInfo) -> ! { loop {} }

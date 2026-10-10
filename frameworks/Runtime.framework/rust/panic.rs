// Userland-only panic handler: deliver SIGABRT for crashd diagnostics.

unsafe extern "C" {
    fn nxu_runtime_abort() -> !;
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { nxu_runtime_abort() }
}

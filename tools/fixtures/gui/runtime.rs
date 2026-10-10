#![no_std]
extern crate alloc;
include!(concat!(env!("NXU_RUNTIME_RUST"), "/allocator.rs"));
include!(concat!(env!("NXU_RUNTIME_RUST"), "/panic.rs"));
#[unsafe(no_mangle)]
pub extern "C" fn nxu_rust_allocation_probe() -> bool {
    unsafe {
        let layout = core::alloc::Layout::from_size_align(127, 65536).unwrap();
        let pointer = alloc::alloc::alloc(layout);
        if pointer.is_null() {
            return false;
        }
        if (pointer as usize) & 65535 != 0 {
            alloc::alloc::dealloc(pointer, layout);
            return false;
        }
        core::ptr::write_bytes(pointer, 42, 127);
        let next = alloc::alloc::realloc(pointer, layout, 255);
        if next.is_null() {
            alloc::alloc::dealloc(pointer, layout);
            return false;
        }
        let intact = core::slice::from_raw_parts(next, 127)
            .iter()
            .all(|x| *x == 42);
        alloc::alloc::dealloc(
            next,
            core::alloc::Layout::from_size_align(255, 65536).unwrap(),
        );
        intact
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn nxu_rust_panic_probe() -> ! {
    panic!("GUI runtime panic probe");
}

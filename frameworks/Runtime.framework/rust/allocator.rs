// Userland-only allocator adapter. Never include this in a kernel framework.
use core::alloc::{GlobalAlloc, Layout};

struct RuntimeAllocator;

unsafe extern "C" {
    fn nxu_alloc(size: u64, alignment: u64) -> *mut u8;
    fn nxu_free(address: *mut u8) -> bool;
    fn nxu_realloc(address: *mut u8, size: u64, alignment: u64) -> *mut u8;
}

unsafe impl GlobalAlloc for RuntimeAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { nxu_alloc(layout.size() as u64, layout.align() as u64) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, _layout: Layout) {
        unsafe {
            let _ = nxu_free(pointer);
        }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        unsafe { nxu_realloc(pointer, size as u64, layout.align() as u64) }
    }
}

#[global_allocator]
static RUNTIME_ALLOCATOR: RuntimeAllocator = RuntimeAllocator;

#![no_std]
// Used for atom linkage
#![feature(linkage)]
// Used for syntax sugar
#![feature(let_else)]
// Used for the `unlikely` compiler hint
#![feature(core_intrinsics)]
// Used for custom allocators
#![feature(allocator_api)]
#![feature(alloc_layout_extra)]
#![feature(layout_for_ptr)]
// Used in process heap impl
#![feature(nonnull_slice_from_raw_parts)]
#![feature(slice_ptr_get)]
#![feature(slice_ptr_len)]
// Used for access to pointer metadata
#![feature(ptr_metadata)]
#![feature(pointer_byte_offsets)]
// Used for implementing Fn, etc. for GcBox, as
// well as interop with closures produced by the compiler
#![feature(fn_traits)]
#![feature(unboxed_closures)]
// Used for const TypeId::of::<T>()
#![feature(const_type_id)]
// Used for NonNull::as_uninit_mut
#![feature(ptr_as_uninit)]
// Used for Arc::get_mut_unchecked
#![feature(get_mut_unchecked)]
// The following are used for the Tuple implementation
#![feature(trusted_len)]
#![feature(slice_index_methods)]
#![feature(exact_size_is_empty)]
// Specialization
#![feature(min_specialization)]
// Used for FFI
#![feature(extern_types)]
#![feature(c_unwind)]
#![feature(arbitrary_enum_discriminant)]
#![cfg_attr(test, feature(test))]
// Used for ErlangResult
#![feature(try_trait_v2)]
#![feature(try_trait_v2_residual)]
#![feature(const_trait_impl)]
#![feature(const_mut_refs)]
// Testing
#![feature(assert_matches)]

extern crate alloc;
#[cfg(feature = "std")]
extern crate std;
#[cfg(test)]
extern crate test;

pub mod backtrace;
pub mod cmp;
pub mod error;
pub mod function;
pub mod intrinsics;
pub mod process;
pub mod term;

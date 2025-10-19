// Stub translation unit for the anything executable.
// The actual implementation (including wmain) lives in the anything_core
// static library so that plugins can link against the shared functionality.
// This file exists solely to satisfy CMake's requirement that executables
// have at least one source file.
void anything_main_stub_reference(void) {}

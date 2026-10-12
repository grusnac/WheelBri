#pragma once

#include "DiFormat.hpp"   // <dinput.h> with DIRECTINPUT_VERSION set

#include <exception>
#include <new>

namespace wheelbri::shim {

// Logs an exception caught at the boundary to the game. Never throws (if even logging fails, nothing is logged).
void noteEscapedException(const char* where, const char* what) noexcept;

// Every COM method, export and enumeration callback runs its body through one of these: an exception must never
// reach the game (it would end in std::terminate, or unwind through the real dinput8.dll's frames). The game gets
// an error code instead: E_OUTOFMEMORY for an allocation that failed, DIERR_GENERIC for anything else.
template <typename Body>
HRESULT guarded(const char* where, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        noteEscapedException(where, "out of memory");
        return E_OUTOFMEMORY;
    } catch (const std::exception& e) {
        noteEscapedException(where, e.what());
        return DIERR_GENERIC;
    } catch (...) {
        noteEscapedException(where, "unknown exception");
        return DIERR_GENERIC;
    }
}

// The same for methods that return something else (an enumeration callback, a pointer): `fallback` on an exception.
template <typename Result, typename Body>
Result guardedOr(const Result fallback, const char* where, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& e) {
        noteEscapedException(where, e.what());
    } catch (...) {
        noteEscapedException(where, "unknown exception");
    }
    return fallback;
}

// For work that must not change what the game gets once the real call is done (a log line after QueryInterface,
// cleanup in Release): an exception is noted and dropped.
template <typename Body>
void guardedVoid(const char* where, Body&& body) noexcept {
    try {
        body();
    } catch (const std::exception& e) {
        noteEscapedException(where, e.what());
    } catch (...) {
        noteEscapedException(where, "unknown exception");
    }
}

}  // namespace wheelbri::shim

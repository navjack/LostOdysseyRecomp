#pragma once

// Windows: installs last-resort SEH, std::terminate and SIGABRT reports.
// Fast-fail, external termination and an unusable process/OS remain outside
// this best-effort diagnostic path. Call once during process initialization.
// POSIX: reports SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT and std::terminate with
// guest address and register context, then re-raises for the system report.
// Only the installing thread gets an alternate signal stack.
void InstallCrashHandler();

// Optional subsystem report written after the guest registers of a fault. Runs in the
// crash path: no allocation, locks or formatting library; write with
// os::logger::EmergencyWrite. At most four reporters; extra registrations are ignored.
using CrashReporter = void (*)() noexcept;
void RegisterCrashReporter(CrashReporter reporter) noexcept;

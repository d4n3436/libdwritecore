//+--------------------------------------------------------------------------
//
//  fault_report.h - report where a crash happened, from inside the process.
//
//  Writes the faulting address and a frame walk, each address as module plus
//  file offset, using only async-signal-safe calls. Then restores the previous
//  handler and returns, so the faulting instruction runs again and the host's
//  own crash handling takes it as it would have. Off unless DWC_FAULT_REPORT
//  is set.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_FAULT_REPORT_H_INCLUDED
#define CHROMIUM_FAULT_REPORT_H_INCLUDED

namespace fault_report {

void InstallAtLoad();

// Install again if something has taken the handler since. The host installs
// its own crash handling after the library is loaded, which replaces this one,
// so a renderer needs it put back once it is running. Cheap after the first
// call and safe from any thread.
void Ensure();

}  // namespace fault_report

#endif  // CHROMIUM_FAULT_REPORT_H_INCLUDED

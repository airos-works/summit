#pragma once
// Crash reports of web processes. The engine's web processes tell Summit when
// they crash (BWebKitCrashReport in WebKitDiagnostics.h) and wait; Summit
// reads every thread's stack with its debugger helper (McpDebugHelper.cpp),
// lets the process end, saves the report in ~/config/var/log/Summit and
// tells the windows (kCrashReportReady), whose crash page asks whether to
// send it. Sending opens GitHub's new-issue form for airos-works/summit with
// the report filled in; nothing leaves the machine unless the user submits
// that form.
#include <string>
#include <vector>

namespace summit {

// Before the engine starts a web process: from then on they report their
// crashes here. SUMMIT_CRASH_REPORTS=0 leaves crashes to debug_server.
void StartCrashReporter();

// GitHub's new-issue form for a report: its title, its crash lines and as
// much of the crashed thread's stack as the address allows (about 6 KB:
// GitHub turns longer ones away), the site's origin (never the whole
// address: the issue is public) and where the full report is, to attach.
std::string CrashIssueURL(const std::string& title, const std::string& heading, const std::vector<std::string>& frames,
    const std::string& origin, const std::string& path);

}

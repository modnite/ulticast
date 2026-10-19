#pragma once
#include <string>

// Diagnostics: persistent log files and a redacted system report for bug reports.
// Everything lands in %LOCALAPPDATA%\Ulticast so users can zip it and attach it
// to a GitHub issue.
namespace Diag {

// Creates the log file for this process run (pruning old logs). Call once, early.
void StartLog(const std::string& appTag);

// Tee a line to stdout and the log file (thread-safe, flushed per line so the
// tail survives a crash). Accepts printf-style formatting.
void Log(const char* fmt, ...);

std::string LogDir();   // %LOCALAPPDATA%\Ulticast\logs
std::string LogPath();  // current run's log file (empty before StartLog)
std::string DataDir();  // %LOCALAPPDATA%\Ulticast

// System + device + (redacted) settings summary, with the log tail appended.
// Safe to show to strangers: the stream key is never included.
std::string BuildReport();

// The last |maxLines| lines of this run's log (indented), for pasting into a
// chat or issue when a full report is overkill.
std::string LogTail(size_t maxLines);

}  // namespace Diag

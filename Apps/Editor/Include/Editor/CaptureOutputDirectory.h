#pragma once

#include <filesystem>

namespace GameEngine::Editor
{

// The directory this editor process writes debug-server capture files into:
// take_screenshot PNGs and capture_resource images.
//
// Process-scoped, and that is the whole point. The file names are derived from a
// per-process sequence number and from resource names, so two editors sharing one
// directory generate identical names and overwrite each other's frames with no
// error anywhere — a harness then reads another session's pixels at the path its
// own call returned. The path carries the process id, so concurrent editors on
// one host cannot name the same file (a TEMP redirected to a share crosses
// hosts and reopens the race).
//
// The directory is emptied on first use in each process: process ids are recycled
// and the capture sequence restarts at 1 in every process, so a dead editor's
// frames would otherwise already sit under the exact names this process is about
// to hand out.
//
// Created on every call, so a directory removed mid-session (a temp sweep) comes
// back; if creation itself fails the writers report it on the response.
std::filesystem::path CaptureOutputDirectory();

} // namespace GameEngine::Editor

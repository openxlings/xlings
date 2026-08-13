// xlings.agent.text_renderer — plain-text rendering for --agent mode.
//
// Converts DataEvent payloads to clean, unformatted text output.
// No ANSI codes, no TUI decorations, no progress bar animations.
// Designed to be easily readable by LLM agents.

export module xlings.agent.text_renderer;

import std;

import xlings.runtime.event;

namespace xlings::agent {

// Render a DataEvent as plain text to stdout.
// Silently skips event kinds that are purely visual (e.g. download_progress).
export void render_data_event(const DataEvent& e);

}  // namespace xlings::agent

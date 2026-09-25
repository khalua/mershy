// Streams a chat reply from Claude over the Messages API (raw HTTPS + SSE +
// cJSON). Keeps a short rolling conversation history in RAM.
#ifndef CLAUDE_CLIENT_H
#define CLAUDE_CLIENT_H

#include <functional>
#include <string>

enum class ClaudeResult { kOk, kError, kRefusal, kCancelled };

// Sends `user_text` with the running history and calls on_text with each
// text fragment as it's generated (raw, including the leading mood/volume
// tags). `cancelled` is polled per fragment; returning true stops early.
// On kOk both turns are appended to the history; otherwise it's unchanged.
ClaudeResult claude_chat_stream(const std::string &user_text,
                                const std::function<void(const std::string &)> &on_text,
                                const std::function<bool()> &cancelled);

void claude_reset_history();

// Opens the connection ahead of time (see HttpConn::warm).
void claude_warm();

#endif  // CLAUDE_CLIENT_H

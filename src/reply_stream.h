// Streams Claude's reply in a background task and hands it out in speakable
// pieces, so speech can start after the first sentence instead of after the
// whole reply.
//
// Claude starts each reply with tags ("[happy] [volume up] ..."); they're
// parsed off the front as they arrive and exposed via header().
#ifndef REPLY_STREAM_H
#define REPLY_STREAM_H

#include <string>

#include "claude_client.h"

struct ReplyHeader {
    std::string mood;              // empty if untagged
    bool has_volume = false;
    bool volume_relative = false;  // true: `volume` is a delta
    int volume = 0;
};

// Starts streaming the reply to `user_text`. Only one stream at a time.
// on_searching is called (from the stream task) when a web search starts
// (true) and when text resumes (false).
void reply_stream_start(const std::string &user_text, void (*on_searching)(bool));

// Blocks until the leading tags are parsed (or the stream ended), then
// fills `out`. Returns false if the stream failed before producing text.
bool reply_stream_header(ReplyHeader *out);

// Blocks until the next piece is ready: the first complete sentence first,
// then everything complete so far. Returns false once the reply is fully
// handed out.
bool reply_stream_next(std::string *out);

// Stops the stream early (e.g. the user tapped to interrupt).
void reply_stream_cancel();

// Waits for the background task to finish; returns how the stream ended.
ClaudeResult reply_stream_finish();

#endif  // REPLY_STREAM_H

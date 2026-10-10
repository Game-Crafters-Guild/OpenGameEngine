#pragma once

#include <string_view>

namespace GameEngine::WebLibrary
{

/// True, with the last error naming `call`, while a chunk query is running (between
/// ge_query_begin and the ge_query_next_chunk that returns 0, or ge_query_end). The exports
/// that change the world's structure or run a frame call it first: either would move or free
/// the chunks whose memory the page is reading and writing.
bool RefuseWhileQueryRuns(std::string_view call);

/// Ends a running query and forgets every cached one: the world they iterate is going away
/// (ge_shutdown, which a page may call from inside a query's callback).
void ReleaseQueries();

} // namespace GameEngine::WebLibrary

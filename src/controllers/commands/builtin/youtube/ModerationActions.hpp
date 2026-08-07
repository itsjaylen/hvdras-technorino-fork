#pragma once

class QString;

namespace chatterino {

struct CommandContext;

}  // namespace chatterino

namespace chatterino::commands {

/// /ban (YouTube)
QString doYouTubeBan(const CommandContext &ctx);

/// /timeout (YouTube)
QString doYouTubeTimeout(const CommandContext &ctx);

/// /unban (YouTube)
QString doYouTubeUnban(const CommandContext &ctx);

/// /delete (YouTube)
QString doYouTubeDelete(const CommandContext &ctx);

}  // namespace chatterino::commands

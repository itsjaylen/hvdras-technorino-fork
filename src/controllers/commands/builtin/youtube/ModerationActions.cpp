#include "controllers/commands/builtin/youtube/ModerationActions.hpp"

#include "common/Channel.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "util/Helpers.hpp"

#include <QString>

#include <chrono>
#include <optional>

namespace {

using namespace Qt::Literals;
using namespace chatterino;

/// YouTube provides no way to resolve a display name to a channel ID (no
/// exact-match lookup API), so ban/timeout/unban only work when triggered
/// from a message or usercard that already carries the channel ID -
/// encoded here as "id:<channelId>", the same convention Kick's commands
/// use for pre-resolved IDs.
std::optional<QString> parseTargetChannelId(const QString &spec)
{
    if (!spec.startsWith(u"id:"))
    {
        return std::nullopt;
    }
    auto id = spec.sliced(3);
    if (id.isEmpty())
    {
        return std::nullopt;
    }
    return id;
}

void doBan(const CommandContext &ctx,
          std::optional<std::chrono::seconds> duration, const QString &usage)
{
    if (!ctx.youtubeChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in YouTube channels"_s);
        return;
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(usage);
        return;
    }

    auto targetChannelId = parseTargetChannelId(ctx.words.at(1));
    if (!targetChannelId)
    {
        ctx.channel->addSystemMessage(
            u"YouTube bans/timeouts can only be done from a message's "
            u"right-click menu or a user's card, not by typing a name - "
            u"YouTube provides no way to look up a channel from a display "
            u"name alone."_s);
        return;
    }

    auto weak = ctx.youtubeChannel->weakFromThis();
    auto id = *targetChannelId;
    ctx.youtubeChannel->resolveLiveChatInfo(
        [weak, id, duration](const ExpectedStr<YouTubeLiveChatInfo> &liveChatIdRes) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (!liveChatIdRes)
            {
                self->addSystemMessage(u"Failed to ban/timeout: " %
                                       liveChatIdRes.error());
                return;
            }

            getYouTubeApi()->banUser(
                liveChatIdRes->liveChatId, id, duration,
                [weak, id, duration](const ExpectedStr<QString> &banRes) {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    if (!banRes)
                    {
                        self->addSystemMessage(u"Failed to ban/timeout: " %
                                               banRes.error());
                        return;
                    }
                    self->recordBan(id, *banRes, duration);
                });
        });
}

}  // namespace

namespace chatterino::commands {

QString doYouTubeBan(const CommandContext &ctx)
{
    doBan(ctx, std::nullopt,
         u"Usage: \"/ban id:<channelId>\" - only usable from a message's "
         u"right-click menu or a user's card."_s);
    return {};
}

QString doYouTubeTimeout(const CommandContext &ctx)
{
    if (!ctx.youtubeChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in YouTube channels"_s);
        return {};
    }

    std::chrono::seconds duration(600);
    if (ctx.words.size() >= 3)
    {
        auto seconds = parseDurationToSeconds(ctx.words.at(2), 1);
        if (seconds <= 0)
        {
            ctx.channel->addSystemMessage(u"Invalid duration."_s);
            return {};
        }
        duration = std::chrono::seconds(seconds);
    }

    doBan(ctx, duration,
         u"Usage: \"/timeout id:<channelId> [duration]\" - only usable from "
         u"a message's right-click menu or a user's card."_s);
    return {};
}

QString doYouTubeDelete(const CommandContext &ctx)
{
    if (!ctx.youtubeChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in YouTube channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(u"Usage: \"/delete <msg.id>\""_s);
        return {};
    }

    // Unlike Twitch/Kick, deleting needs more than just the message ID (see
    // YouTubeChannel::deleteMessage) - recovered here by looking the message
    // up in our own local history instead of needing it passed in directly,
    // so {msg.id}-based moderation action buttons still work.
    auto msg = ctx.channel->findMessageByID(ctx.words.at(1));
    if (!msg)
    {
        ctx.channel->addSystemMessage(
            u"Couldn't find that message to delete it - it may have "
            u"scrolled out of the local history."_s);
        return {};
    }

    ctx.youtubeChannel->deleteMessage(msg->id, msg->userID,
                                      msg->serverReceivedTime,
                                      msg->messageText);
    return {};
}

QString doYouTubeUnban(const CommandContext &ctx)
{
    if (!ctx.youtubeChannel)
    {
        ctx.channel->addSystemMessage(
            u"This command only works in YouTube channels"_s);
        return {};
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(u"Usage: \"/unban id:<channelId>\""_s);
        return {};
    }

    auto targetChannelId = parseTargetChannelId(ctx.words.at(1));
    if (!targetChannelId)
    {
        ctx.channel->addSystemMessage(
            u"YouTube unbans can only be done from a user's card, not by "
            u"typing a name."_s);
        return {};
    }

    auto banId = ctx.youtubeChannel->takeBanId(*targetChannelId);
    if (!banId)
    {
        ctx.channel->addSystemMessage(
            u"No ban/timeout for this user was recorded in this session - "
            u"unban them directly from YouTube Studio instead."_s);
        return {};
    }

    auto weak = ctx.youtubeChannel->weakFromThis();
    getYouTubeApi()->unbanUser(*banId, [weak](const ExpectedStr<void> &res) {
        auto self = weak.lock();
        if (!self || res)
        {
            return;
        }
        self->addSystemMessage(u"Failed to unban: " % res.error());
    });
    return {};
}

}  // namespace chatterino::commands

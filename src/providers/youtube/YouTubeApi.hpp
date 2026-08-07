// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

namespace chatterino {

struct YouTubeLiveChatInfo {
    QString liveChatId;
    /// The channel ID of the broadcaster, i.e. the owner of the video -
    /// resolved in the same call as liveChatId since both come from the
    /// same videos.list response, so checking "am I the broadcaster" never
    /// costs a separate request.
    QString broadcasterChannelId;
};

/// A minimal client for the write endpoints of the official YouTube Data
/// API v3 that this app needs for moderation. Unlike YouTubeChannel (which
/// reads live chat anonymously via the unofficial Innertube API), these
/// calls require an authenticated YouTubeAccount's OAuth token.
class YouTubeApi
{
public:
    template <typename T>
    using Callback = std::function<void(ExpectedStr<T>)>;

    static YouTubeApi *instance();

    /// Resolves a video ID to its currently active live chat ID and
    /// broadcaster channel ID (videos.list?part=snippet,liveStreamingDetails).
    void getLiveChatInfo(const QString &videoId,
                        Callback<YouTubeLiveChatInfo> cb);

    /// Checks whether a channel ID appears in a live chat's moderator list
    /// (liveChatModerators.list). Only looks at the first page of results -
    /// channels with more than 50 moderators could miss a match, which is
    /// an accepted edge case here. Note this may not be callable by a
    /// moderator's own account depending on YouTube's API restrictions -
    /// an error here should be treated as "unknown", not "not a moderator".
    void checkIsModerator(const QString &liveChatId, const QString &channelId,
                          Callback<bool> cb);

    /// The IDs read from YouTube's unofficial live chat feed (used for
    /// free, anonymous reading) aren't valid liveChatMessages resource IDs
    /// for the official Data API - they live in a different namespace. To
    /// delete a message we saw there, we instead look it up in the
    /// official API's own recent-messages list by author/timestamp/text
    /// and use *that* copy's ID.
    void findMessageId(const QString &liveChatId,
                       const QString &authorChannelId,
                       const QDateTime &timestamp, const QString &messageText,
                       Callback<QString> cb);

    void deleteMessageById(const QString &messageId, Callback<void> cb);

    /// Bans (duration = nullopt) or times out (duration = a length) a user
    /// from a live chat. On success, the callback receives YouTube's ban
    /// resource ID - the *only* way to undo it later, since the API has no
    /// unban-by-channel-ID endpoint.
    void banUser(const QString &liveChatId, const QString &targetChannelId,
                std::optional<std::chrono::seconds> duration,
                Callback<QString> cb);

    /// Undoes a ban/timeout, given the ban resource ID returned by banUser().
    void unbanUser(const QString &banId, Callback<void> cb);

    void setAuth(const QString &authToken);

private:
    YouTubeApi() = default;

    QByteArray authToken_;
};

YouTubeApi *getYouTubeApi();

}  // namespace chatterino

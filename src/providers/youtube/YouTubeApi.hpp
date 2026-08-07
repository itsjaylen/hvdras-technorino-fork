// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <functional>

namespace chatterino {

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

    /// Resolves a video ID to its currently active live chat ID
    /// (videos.list?part=liveStreamingDetails).
    void getLiveChatId(const QString &videoId, Callback<QString> cb);

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

    void setAuth(const QString &authToken);

private:
    YouTubeApi() = default;

    QByteArray authToken_;
};

YouTubeApi *getYouTubeApi();

}  // namespace chatterino

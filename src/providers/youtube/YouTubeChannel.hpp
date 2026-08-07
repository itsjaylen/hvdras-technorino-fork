// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QString>

#include <memory>

namespace chatterino {

/// Read-only YouTube live chat channel.
/// Polls the YouTube internal live chat API without requiring an API key.
/// Open via channel type "YouTube" with channelName = video ID.
class YouTubeChannel final : public Channel
{
public:
    explicit YouTubeChannel(const QString &videoId);
    ~YouTubeChannel() override;

    // Must be called once, immediately after the shared_ptr is created.
    void initialize();

    const QString &videoId() const;

    /// The stream's title, if known. Populated once when the watch/live page
    /// is first fetched; not kept up to date afterwards.
    const QString &title() const;
    /// URL of the stream's thumbnail image, if known. Same freshness caveat
    /// as title().
    const QString &thumbnailUrl() const;

    bool canSendMessage() const override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    bool hasModRights() const override;

    /// Deletes a message via the official YouTube Data API v3, using the
    /// currently logged-in YouTubeAccount's OAuth token. Requires that
    /// account to actually be a moderator/owner of this chat - otherwise
    /// the request fails and a system message is posted with the error.
    ///
    /// The message's own ID (as seen from the unofficial live chat feed
    /// this channel reads from) isn't a valid ID for the official API, so
    /// it's looked up by author/timestamp/text first - see
    /// YouTubeApi::findMessageId.
    void deleteMessage(const QString &authorChannelId,
                       const QDateTime &timestamp, const QString &messageText);

    /// Fired whenever isLive() changes, so the tab's live indicator updates.
    pajlada::Signals::NoArgSignal liveStatusChanged;

private:
    void fetchChannelLivePage(const QString &handle);
    void fetchWatchPage();
    void fetchLiveChat(const QString &continuation);
    void scheduleNextPoll(const QString &continuation, int timeoutMs);
    void setLive(bool live);
    /// Called when the chat/stream we were watching ends (or a handle
    /// lookup finds nobody currently live). Schedules another attempt to
    /// find a live stream instead of giving up permanently.
    void scheduleRediscovery();

    QString videoId_;
    // The owning channel's path (e.g. "@somechannel" or "channel/UCxxxx").
    // Set immediately if this channel was opened via a handle; otherwise
    // learned from the video's watch page once it's fetched. Used to
    // re-search for a new live stream on the same channel after one ends,
    // instead of only ever re-checking a single dead video.
    QString handle_;
    QString apiKey_;
    QString title_;
    QString thumbnailUrl_;
    bool live_ = false;
    // True once the first fetchLiveChat() poll of a connection has been
    // displayed. That first poll is a catch-up batch of messages that
    // already happened, not new arrivals, so it's shown immediately rather
    // than staggered like later polls. Reset whenever a new connection to a
    // live chat starts.
    bool receivedFirstBatch_ = false;
    // Cached activeLiveChatId for the current videoId_, resolved lazily on
    // first moderation action. Cleared whenever videoId_ changes (i.e. a new
    // connection/broadcast is picked up).
    QString liveChatId_;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino

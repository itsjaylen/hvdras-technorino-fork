// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "util/Expected.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

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

    std::shared_ptr<YouTubeChannel> sharedFromThis();
    std::weak_ptr<YouTubeChannel> weakFromThis();

    const QString &videoId() const;

    /// The stream's title, if known. Populated once when the watch/live page
    /// is first fetched; not kept up to date afterwards.
    const QString &title() const;
    /// URL of the stream's thumbnail image, if known. Same freshness caveat
    /// as title().
    const QString &thumbnailUrl() const;
    /// Approximate concurrent viewer count, if known. Same freshness
    /// caveat as title().
    unsigned viewerCount() const;
    /// When the current broadcast started, if known (invalid QDateTime
    /// otherwise). Same freshness caveat as title().
    const QDateTime &streamStartedAt() const;

    bool canSendMessage() const override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    /// Optimistic: true for any logged-in YouTube account until/unless the
    /// real check (see hasConfirmedModRights) comes back negative, so a
    /// single mod/ban/timeout/delete attempt is never blocked client-side -
    /// worst case YouTube's own API rejects it with a real error. Used by
    /// the per-action paths (usercard buttons, right-click menu).
    bool hasModRights() const override;
    /// Strict: only true once the real broadcaster/moderator check (see
    /// refreshModStatus) has positively confirmed it - false both while
    /// still checking and if it's permanently unable to complete. Used to
    /// decide whether to expose broader moderation UI (the moderation-mode
    /// toggle) to begin with, where showing it to a random logged-in
    /// viewer would be misleading even though any action they took through
    /// it would still just fail server-side.
    bool hasConfirmedModRights() const;

    /// Deletes a message via the official YouTube Data API v3, using the
    /// currently logged-in YouTubeAccount's OAuth token. Requires that
    /// account to actually be a moderator/owner of this chat - otherwise
    /// the request fails and a system message is posted with the error.
    ///
    /// `messageId` is the message's own ID, as seen from the unofficial live
    /// chat feed this channel reads from - it isn't a valid ID for the
    /// official API, so the message is looked up there by
    /// author/timestamp/text first (see YouTubeApi::findMessageId).
    /// `messageId` is still used to hide the message locally immediately
    /// after a successful delete, rather than waiting for the next live
    /// chat poll to notice YouTube's own removal event.
    void deleteMessage(const QString &messageId, const QString &authorChannelId,
                       const QDateTime &timestamp, const QString &messageText);

    /// Resolves (and caches) the live chat info for the current broadcast.
    /// Used by every moderation action (delete/ban/timeout/mod check), which
    /// all need it but shouldn't each re-resolve it individually.
    void resolveLiveChatInfo(
        std::function<void(ExpectedStr<YouTubeLiveChatInfo>)> cb);

    /// Remembers the ban resource ID YouTube returned for a given target
    /// channel, so a later unban can reference it - the official API can
    /// only unban by ban resource ID, not by channel ID, and has no way to
    /// look one up after the fact. `duration` is kept alongside it purely
    /// for local display purposes (see peekBanDuration).
    void recordBan(const QString &targetChannelId, const QString &banId,
                   std::optional<std::chrono::seconds> duration);
    /// Returns and forgets the most recent ban ID recorded for a channel in
    /// this session, if any. Used by unban.
    std::optional<QString> takeBanId(const QString &targetChannelId);
    /// Non-consuming lookup of the most recent ban/timeout we issued for this
    /// channel in this session. Outer nullopt = no record found (action was
    /// taken elsewhere - YouTube Studio, another mod, a previous session -
    /// or this channel was never banned by us); inner nullopt = a permanent
    /// ban; a value = a timeout of that length. YouTube's live chat feed
    /// doesn't say which of these a removeChatItemByAuthorAction was, so
    /// this only ever helps for actions this session itself took.
    std::optional<std::optional<std::chrono::seconds>> peekBanDuration(
        const QString &targetChannelId) const;

    /// Fired whenever isLive() changes, so the tab's live indicator updates.
    pajlada::Signals::NoArgSignal liveStatusChanged;
    /// Fired once the real moderator-status check kicked off by
    /// hasModRights()/refreshModStatus() resolves, so UI that already
    /// rendered based on the optimistic fallback can re-check.
    pajlada::Signals::NoArgSignal modStatusChanged;
    /// Fired whenever viewerCount()/streamStartedAt() are refreshed (see
    /// scheduleStatsRefresh), so the tab tooltip can pick up the new
    /// values instead of staying frozen at whatever they were on connect.
    pajlada::Signals::NoArgSignal streamStatusChanged;

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
    /// Kicks off (at most once per broadcast) a real check of whether the
    /// currently logged-in YouTube account is the broadcaster or a
    /// moderator of this specific chat, via videos.list +
    /// liveChatModerators.list. Until/unless this resolves, hasModRights()
    /// falls back to assuming yes for any logged-in account, same as
    /// before this existed - the check may simply be unable to complete
    /// (e.g. if YouTube restricts liveChatModerators.list for non-owner
    /// accounts, which hasn't been confirmed either way).
    void refreshModStatus();
    /// Re-fetches just enough of the watch page to refresh viewerCount()/
    /// streamStartedAt(), then reschedules itself - separate from
    /// fetchWatchPage()/fetchChannelLivePage() so it never touches the
    /// live chat connection itself. Runs on a timer started when the
    /// stream goes live, stopped implicitly once it stops (the weak_ptr
    /// check simply won't reschedule once live_ is false).
    void scheduleStatsRefresh();
    void refreshStreamStats();

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
    unsigned viewerCount_ = 0;
    QDateTime streamStartedAt_;
    bool live_ = false;
    // True once the first fetchLiveChat() poll of a connection has been
    // displayed. That first poll is a catch-up batch of messages that
    // already happened, not new arrivals, so it's shown immediately rather
    // than staggered like later polls. Reset whenever a new connection to a
    // live chat starts.
    bool receivedFirstBatch_ = false;
    // Cached live chat info for the current videoId_, resolved lazily on
    // first moderation action. Cleared whenever videoId_ changes (i.e. a new
    // connection/broadcast is picked up).
    QString liveChatId_;
    QString broadcasterChannelId_;
    // Real moderator-status result, if the check managed to complete for
    // the current broadcast - see refreshModStatus(). mutable because
    // hasModRights() (const) lazily triggers the check as a side effect.
    mutable std::optional<bool> confirmedModRights_;
    mutable bool checkingModRights_ = false;
    // Set the moment a check is kicked off and never cleared until the
    // next broadcast, regardless of outcome - without this, an
    // inconclusive result (confirmedModRights_ staying unset) would let
    // every subsequent hasModRights() call re-trigger the check from
    // scratch, burning API quota on every single call site that happens
    // to invoke it (usercards, context menus, SplitHeader icon refreshes,
    // etc.) instead of at most once per broadcast as intended.
    mutable bool attemptedModStatusCheck_ = false;
    struct RecordedBan {
        QString banId;
        std::optional<std::chrono::seconds> duration;
    };
    // Ban records from successful ban/timeout calls, keyed by target channel
    // ID. In-memory only and scoped to the current liveChatId_ - cleared
    // alongside it.
    QHash<QString, RecordedBan> bansByChannelId_;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino

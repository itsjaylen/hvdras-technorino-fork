// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChatServer.hpp"

#include "providers/youtube/YouTubeChannel.hpp"

#include <vector>

namespace chatterino {

std::shared_ptr<YouTubeChannel> YouTubeChatServer::find(
    const QString &videoId) const
{
    auto it = this->channelsByVideoId_.find(videoId);
    if (it != this->channelsByVideoId_.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<Channel> YouTubeChatServer::getOrCreate(
    const QString &videoId)
{
    if (auto existing = this->find(videoId))
    {
        return existing;
    }

    // The exact-key lookup above only catches channels opened with the
    // *identical* identifier string. A channel opened by handle and the
    // same broadcast opened by its raw video ID look like two different
    // keys even though they're the same live stream - without this, both
    // would get their own independent YouTubeChannel, each polling
    // YouTube's live chat endpoint separately for the same broadcast,
    // which can trigger errors/rate limiting on one or both. Fall back to
    // matching against each still-alive channel's already-resolved
    // identity (its current video ID or learned handle) before creating a
    // new one. Also opportunistically drops entries for closed channels,
    // since nothing else prunes this map.
    QString normalizedHandle =
        videoId.startsWith(u'@') ? videoId : u'@' + videoId;
    std::shared_ptr<YouTubeChannel> matched;
    std::vector<QString> staleKeys;
    for (const auto &[key, weak] : this->channelsByVideoId_)
    {
        auto existing = weak.lock();
        if (!existing)
        {
            staleKeys.push_back(key);
            continue;
        }
        if (!matched &&
            (existing->videoId() == videoId ||
             (!existing->handle().isEmpty() &&
              existing->handle() == normalizedHandle)))
        {
            matched = existing;
        }
    }
    for (const auto &key : staleKeys)
    {
        this->channelsByVideoId_.erase(key);
    }
    if (matched)
    {
        // Register this alias too, so future lookups for this exact
        // string hit the fast path above.
        this->channelsByVideoId_[videoId] = matched;
        return matched;
    }

    auto chan = std::make_shared<YouTubeChannel>(videoId);
    // initialize() must be called after shared_ptr construction so that
    // weak_from_this() is valid inside the network request callbacks.
    chan->initialize();
    this->channelsByVideoId_[videoId] = chan;
    return chan;
}

}  // namespace chatterino

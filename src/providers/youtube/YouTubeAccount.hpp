// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/accounts/Account.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace chatterino {

struct YouTubeAccountData {
    QString channelName;
    // The channel's @handle, without the leading '@' (e.g. "hvdras", not
    // "@hvdras" or "Hydra") - what people actually type to mention someone
    // on YouTube, which can differ from the channel's display name. Used
    // for the self-highlight/ping phrase. May be empty for accounts logged
    // in before this field existed, or if the channel has no handle set.
    QString handle;
    QString channelId;
    QString clientID;
    QString clientSecret;
    QString authToken;
    QString refreshToken;
    QDateTime expiresAt;

    void save() const;
    static std::optional<YouTubeAccountData> loadRaw(const std::string &key);
};

/// An authenticated Google/YouTube account, used to perform moderation
/// actions (delete messages, ban/timeout users) via the official YouTube
/// Data API v3. Unrelated to the anonymous, unauthenticated live chat
/// reading in YouTubeChannel - this is only needed for write actions.
class YouTubeAccount : public Account,
                       public std::enable_shared_from_this<YouTubeAccount>
{
public:
    explicit YouTubeAccount(const YouTubeAccountData &args);
    ~YouTubeAccount() override;

    constexpr static std::chrono::minutes CHECK_REFRESH_INTERVAL{2};

    Q_DISABLE_COPY_MOVE(YouTubeAccount);

    void save() const;

    bool update(const YouTubeAccountData &data);

    QString toString() const override;

    bool isAnonymous() const
    {
        return this->channelId_.isEmpty();
    }

    QString channelName() const
    {
        return this->channelName_;
    }
    QString handle() const
    {
        return this->handle_;
    }
    QString channelId() const
    {
        return this->channelId_;
    }
    QString clientID() const
    {
        return this->clientID_;
    }
    QString clientSecret() const
    {
        return this->clientSecret_;
    }
    QString authToken() const
    {
        return this->authToken_;
    }

    void refreshIfNeeded();

    pajlada::Signals::NoArgSignal authUpdated;

private:
    void doRefresh();

    QString channelName_;
    QString handle_;
    QString channelId_;
    QString clientID_;
    QString clientSecret_;
    QString authToken_;
    QString refreshToken_;
    QDateTime expiresAt_;
};

}  // namespace chatterino

// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/SignalVector.hpp"

#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QString>
#include <QTimer>

namespace chatterino {

class YouTubeAccount;
struct YouTubeAccountData;

class YouTubeAccountManager
{
public:
    YouTubeAccountManager();

    std::shared_ptr<YouTubeAccount> current();

    std::vector<QString> channelNames() const;

    std::shared_ptr<YouTubeAccount> findUserByChannelId(
        const QString &channelId) const;
    bool userExists(const QString &channelId) const;

    void reloadUsers();
    void load();

    bool isLoggedIn() const;

    pajlada::Settings::Setting<QString> currentChannelId{
        "/youtubeAccounts/current", ""};

    pajlada::Signals::NoArgSignal currentUserChanged;
    pajlada::Signals::NoArgSignal userListUpdated;

    SignalVector<std::shared_ptr<YouTubeAccount>> accounts;

private:
    enum class AddUserResponse : uint8_t {
        UserAlreadyExists,
        UserUpdated,
        UserAdded,
    };
    AddUserResponse addAccount(const YouTubeAccountData &data);
    bool removeAccount(YouTubeAccount *account);

    void refreshAccounts() const;

    std::shared_ptr<YouTubeAccount> currentUser_;
    std::shared_ptr<YouTubeAccount> anonymousUser_;
    QTimer refreshTimer;
    pajlada::Signals::SignalHolder holder;
};

}  // namespace chatterino

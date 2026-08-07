// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeAccountManager.hpp"

#include "common/QLogging.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep
#include "util/SharedPtrElementLess.hpp"

#include <pajlada/settings/setting.hpp>

namespace chatterino {

YouTubeAccountManager::YouTubeAccountManager()
    : accounts(SharedPtrElementLess<YouTubeAccount>{})
    , anonymousUser_(std::make_shared<YouTubeAccount>(YouTubeAccountData{}))
{
    std::ignore = this->accounts.itemRemoved.connect([this](const auto &acc) {
        this->removeAccount(acc.item.get());
    });

    this->refreshTimer.setSingleShot(false);
    this->refreshTimer.setInterval(YouTubeAccount::CHECK_REFRESH_INTERVAL);
    // NOLINTNEXTLINE(clazy-connect-3arg-lambda)
    QObject::connect(&this->refreshTimer, &QTimer::timeout, [this] {
        this->refreshAccounts();
    });
    this->refreshTimer.start();
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::current()
{
    if (!this->currentUser_)
    {
        return this->anonymousUser_;
    }
    return this->currentUser_;
}

std::vector<QString> YouTubeAccountManager::channelNames() const
{
    std::vector<QString> names;
    for (const auto &acc : this->accounts.raw())
    {
        names.emplace_back(acc->channelName());
    }
    return names;
}

std::shared_ptr<YouTubeAccount> YouTubeAccountManager::findUserByChannelId(
    const QString &channelId) const
{
    for (const auto &acc : this->accounts.raw())
    {
        if (acc->channelId() == channelId)
        {
            return acc;
        }
    }
    return nullptr;
}

bool YouTubeAccountManager::userExists(const QString &channelId) const
{
    return this->findUserByChannelId(channelId) != nullptr;
}

bool YouTubeAccountManager::isLoggedIn() const
{
    return this->currentUser_ && !this->currentUser_->isAnonymous();
}

void YouTubeAccountManager::reloadUsers()
{
    auto keys =
        pajlada::Settings::SettingManager::getObjectKeys("/youtubeAccounts");

    bool listUpdated = false;

    for (const auto &uid : keys)
    {
        if (uid == "current")
        {
            continue;
        }

        auto data = YouTubeAccountData::loadRaw(uid);
        if (!data)
        {
            continue;
        }

        switch (this->addAccount(*data))
        {
            case AddUserResponse::UserAlreadyExists: {
                qCDebug(chatterinoYoutube)
                    << "User" << data->channelName << "already exists";
            }
            break;
            case AddUserResponse::UserUpdated: {
                qCDebug(chatterinoYoutube)
                    << "User" << data->channelName << "updated";
                if (data->channelId == this->current()->channelId())
                {
                    this->currentUserChanged.invoke();
                }
            }
            break;
            case AddUserResponse::UserAdded: {
                qCDebug(chatterinoYoutube)
                    << "Added account" << data->channelName;
                listUpdated = true;
            }
            break;
        }
    }

    if (listUpdated)
    {
        this->userListUpdated.invoke();
        this->refreshAccounts();
    }
}

void YouTubeAccountManager::load()
{
    this->reloadUsers();

    this->currentChannelId.connect([this](const QString &newChannelId) {
        auto user = this->findUserByChannelId(newChannelId);
        if (user)
        {
            qCDebug(chatterinoYoutube)
                << "YouTube user updated to" << user->channelName();
            getYouTubeApi()->setAuth(user->authToken());
            this->currentUser_ = user;
        }
        else
        {
            qCDebug(chatterinoYoutube) << "YouTube user updated to anonymous";
            this->currentUser_ = this->anonymousUser_;
        }

        this->currentUserChanged.invoke();
    });
}

YouTubeAccountManager::AddUserResponse YouTubeAccountManager::addAccount(
    const YouTubeAccountData &data)
{
    auto previousUser = this->findUserByChannelId(data.channelId);
    if (previousUser)
    {
        bool userUpdated = previousUser->update(data);
        if (userUpdated)
        {
            return AddUserResponse::UserUpdated;
        }

        return AddUserResponse::UserAlreadyExists;
    }

    auto account = std::make_shared<YouTubeAccount>(data);
    this->accounts.insert(account);
    this->holder.managedConnect(account->authUpdated, [this, account] {
        if (this->currentUser_ == account)
        {
            getYouTubeApi()->setAuth(account->authToken());
            qCDebug(chatterinoYoutube)
                << "YouTube auth updated for" << account->channelName();
        }
    });

    return AddUserResponse::UserAdded;
}

bool YouTubeAccountManager::removeAccount(YouTubeAccount *account)
{
    if (account->isAnonymous())
    {
        return false;
    }

    auto accountPath =
        "/youtubeAccounts/uid" + account->channelId().toStdString();
    pajlada::Settings::SettingManager::gRemoveSetting(accountPath);

    if (account->channelId() == this->currentChannelId)
    {
        // The user that was removed is the current user, log into the
        // anonymous account
        this->currentChannelId = "";
    }

    this->userListUpdated.invoke();
    return true;
}

void YouTubeAccountManager::refreshAccounts() const
{
    for (const auto &acc : this->accounts.raw())
    {
        acc->refreshIfNeeded();
    }
}

}  // namespace chatterino

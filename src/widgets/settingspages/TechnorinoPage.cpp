#include "widgets/settingspages/TechnorinoPage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/NativeMessaging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/FuzzyConvert.hpp"
#include "util/Helpers.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDialog>
#include <QLabel>
#include <QScrollArea>

namespace {

using namespace chatterino;
using namespace literals;

#ifdef Q_OS_WIN
const QString META_KEY = u"Windows"_s;
#else
const QString META_KEY = u"Meta"_s;
#endif

void addKeyboardModifierSetting(GeneralPageView &layout, const QString &title,
                                EnumSetting<Qt::KeyboardModifier> &setting)
{
    layout.addDropdown<std::underlying_type<Qt::KeyboardModifier>::type>(
        title, {"None", "Shift", "Control", "Alt", META_KEY}, setting,
        [](int index) {
            switch (index)
            {
                case Qt::ShiftModifier:
                    return 1;
                case Qt::ControlModifier:
                    return 2;
                case Qt::AltModifier:
                    return 3;
                case Qt::MetaModifier:
                    return 4;
                default:
                    return 0;
            }
        },
        [](DropdownArgs args) {
            switch (args.index)
            {
                case 1:
                    return Qt::ShiftModifier;
                case 2:
                    return Qt::ControlModifier;
                case 3:
                    return Qt::AltModifier;
                case 4:
                    return Qt::MetaModifier;
                default:
                    return Qt::NoModifier;
            }
        },
        false);
}
}  // namespace

namespace chatterino {

TechnorinoPage::TechnorinoPage()
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;
    x->addWidget(view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    this->initLayout(*view);

    this->initExtra();
}

bool TechnorinoPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }
    else
    {
        return false;
    }
}

void TechnorinoPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    layout.addTitle("Chat");
    // SettingWidget::checkbox("", s.hideModerated)->setTooltip("")->addTo(layout);
    SettingWidget::checkbox(
        "Show placeholder in text input box (requires restart)",
        s.showTextInputPlaceholder)
        ->addTo(layout);
    SettingWidget::checkbox("Convert #text to channel links", s.channelLinks)
        ->addTo(layout);

    layout.addTitle("Input Box");
    SettingWidget::checkbox("Show command suggestions while typing",
                            s.showCommandSuggestions)
        ->setTooltip("Show a strip of matching /commands above the input as "
                     "you type.")
        ->addTo(layout);
    SettingWidget::checkbox("Hide emoji/emote button", s.hideEmojiButton)
        ->setTooltip("Hide the emote picker button next to the message input.")
        ->addTo(layout);

    layout.addTitle("Client");
    SettingWidget::checkbox("Client detection highlights. ",
                            s.normalNonceDetection)
        ->setTooltip("Highlights messages sent from specified clients "
                     "using the specified color below.")
        ->addTo(layout);
    SettingWidget::colorButton("Webchat color", s.webchatColor)->addTo(layout);
    SettingWidget::colorButton("Android color", s.androidColor)->addTo(layout);
    SettingWidget::colorButton("iOS color", s.iosColor)->addTo(layout);
    SettingWidget::checkbox("Client detection icons. ", s.clientDetectionIcon)
        ->setTooltip("Displays client icons beside messages")
        ->addTo(layout);
    SettingWidget::checkbox("Show Translate message in message menu",
                            s.showTranslateMessageContextAction)
        ->setTooltip("Right-click any chat message to translate it inline. "
                     "Shows (translated) indicator and a \"Show original\" "
                     "option to revert.")
        ->addTo(layout);
    SettingWidget::checkbox("Show translated indicator",
                            s.showTranslatedMessageIndicator)
        ->setTooltip("Append (translated) after translated message text.")
        ->addTo(layout);
    SettingWidget::checkbox("Show translation button in input",
                            s.showOutgoingTranslationButton)
        ->setTooltip("Show a TL button next to the input box that translates "
                     "your message to the target language before sending.")
        ->addTo(layout);
    SettingWidget::checkbox("Auto-translate incoming messages",
                            s.autoTranslateIncomingMessages)
        ->setTooltip("Automatically translate all incoming chat messages "
                     "inline. Uses the same target language as /translate.")
        ->addTo(layout);
    SettingWidget::lineEdit("Translate messages to",
                            s.messageTranslationTargetLanguage,
                            "Language code, e.g. en, fr, ja")
        ->setTooltip("Target language for /translate and right-click "
                     "translate. Use a language code like en, fr, ja, es.")
        ->addTo(layout);

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Fake messages as webchat", s.fakeWebChat)
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for messages",
                            s.useBotLimitsMessage)
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for JOINs", s.useBotLimitsJoin)
        ->addTo(layout);
    SettingWidget::checkbox(
        "Enable. Required for abnormal nonce and webchat detection to work!",
        s.nonceFuckeryEnabled)
        ->addTo(layout);
    SettingWidget::checkbox("Abnormal nonce detection",
                            s.abnormalNonceDetection)
        ->addTo(layout);
    SettingWidget::checkbox("\"7TV User\" usercard button", s.stvUsercardButton)
        ->setTooltip("Add \"7TV User\" button to usercard directly")
        ->addTo(layout);
    SettingWidget::checkbox("Watching tab live sound", s.watchingTabLiveSound)
        ->addTo(layout);
    SettingWidget::checkbox("Auto detach watching tab (~10s timeout)",
                            s.autoDetachLiveTab)
        ->addTo(layout);
    SettingWidget::checkbox("Markdown parsing (Experimental)",
                            s.markdownParsing)
        ->addTo(layout);
    SettingWidget::checkbox(
        "Anon read connection (requires restart) (Experimental)", s.anonRead)
        ->setTooltip("Use an anon connection for the read connection. NOTE: "
                     "This will NOT guarantee exclusion from viewerlists.")
        ->addTo(layout);

    auto addBannerScaleDropdown = [&layout](const QString &label,
                                            auto &setting,
                                            const QString &tooltip) {
        layout.addDropdown<float>(
                  label,
                  {"0.5x", "0.6x", "0.75x", "0.9x", "Default", "1.1x",
                   "1.25x", "1.4x", "1.5x", "1.75x", "2x"},
                  setting,
                  [](float val) {
                      if (val == 1.f)
                      {
                          return QString("Default");
                      }
                      return QString::number(val) + "x";
                  },
                  [](DropdownArgs args) {
                      return fuzzyToFloat(args.value, 1.f);
                  },
                  false)
            ->setToolTip(tooltip);
    };

    layout.addTitle("Pinned Messages");
    layout.addDescription("Pinned message banner and pin action options.");
    SettingWidget::checkbox("Move Pin actions to Moderate menu",
                            s.movePinToModerateMenu)
        ->setTooltip("Put Pin and Unpin inside the Moderate submenu when "
                     "right-clicking a message.")
        ->addTo(layout);
    layout.addDropdown<int>(
        "Show pin button on mods and broadcaster",
        {"Never", "In moderation mode", "Always"},
        s.showPinButtonOnModeratorsMode,
        [](int val) {
            switch (val)
            {
                case 0: return QString("Never");
                case 2: return QString("Always");
                default: return QString("In moderation mode");
            }
        },
        [](DropdownArgs args) {
            if (args.value == "Never") return 0;
            if (args.value == "Always") return 2;
            return 1;
        },
        false)
        ->setToolTip("When to show the inline Pin button beside messages.");
    SettingWidget::checkbox("Show pinned messages",
                            s.enablePinnedMessages)
        ->setTooltip("Show the pinned message banner above chat.")
        ->addTo(layout);
    addBannerScaleDropdown("Pinned message scale", s.pinnedMessageScale,
                           "Make the pinned message banner larger or smaller.");
    SettingWidget::checkbox("Enable /pin <message text>",
                            s.enablePinCommandMessages)
        ->setTooltip("Let /pin followed by text send that message and pin it.")
        ->addTo(layout);
    layout.addDropdown<int>(
        "Default pin duration",
        {"Indefinite", "5 minutes", "10 minutes", "20 minutes", "30 minutes"},
        s.defaultPinDuration,
        [](int val) {
            if (val <= 0) return QString("Indefinite");
            return QString::number(val / 60) + " minutes";
        },
        [](DropdownArgs args) {
            if (args.value == "Indefinite") return -1;
            return args.value.split(' ')[0].toInt() * 60;
        },
        false)
        ->setToolTip("How long pins last when no duration is specified.");
    SettingWidget::checkbox("Show unpin notifications in chat",
                            s.showUnpinNotifications)
        ->setTooltip("Show a system message when someone unpins a message.")
        ->addTo(layout);

    layout.addTitle("Poll and Prediction");
    layout.addDescription(
        "Poll, prediction, and banner behavior options. Only the "
        "broadcaster can create polls and predictions.");
    SettingWidget::checkbox("Show predictions",
                            s.enablePredictions)
        ->setTooltip("Show prediction banners above chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show polls",
                            s.enablePolls)
        ->setTooltip("Show poll banners above chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show prediction chat messages",
                            s.showPredictionSystemMessages)
        ->setTooltip("Show a chat message when predictions are created, "
                     "locked, paid out, or refunded.")
        ->addTo(layout);
    SettingWidget::checkbox("Close prediction menu after betting",
                            s.predictionAutoCloseDialog)
        ->setTooltip("Close the prediction dialog after placing a bet.")
        ->addTo(layout);
    SettingWidget::checkbox("Close poll menu after voting",
                            s.pollAutoCloseDialog)
        ->setTooltip("Close the poll dialog after casting a vote.")
        ->addTo(layout);
    SettingWidget::checkbox("Close poll and prediction menus on focus loss",
                            s.predictionCloseOnFocusLoss)
        ->setTooltip("Close dialogs when you click away from them.")
        ->addTo(layout);
    layout.addDropdown<int>(
        "Auto-dismiss resolved banners",
        {"Never", "After 30 seconds", "After 1 minute", "After 5 minutes",
         "After 10 minutes"},
        s.predictionAutoDismissSeconds,
        [](int val) {
            if (val <= 0) return QString("Never");
            if (val < 60) return QString("After %1 seconds").arg(val);
            return QString("After %1 minute%2")
                .arg(val / 60)
                .arg(val / 60 == 1 ? "" : "s");
        },
        [](DropdownArgs args) {
            if (args.value == "Never") return 0;
            if (args.value.contains("30 seconds")) return 30;
            if (args.value.contains("1 minute")) return 60;
            if (args.value.contains("5 minute")) return 300;
            if (args.value.contains("10 minute")) return 600;
            return 300;
        },
        false)
        ->setToolTip("Hide completed poll and prediction banners after a delay.");
    layout.addDropdown<int>(
        "Banner stacking behavior",
        {"Show all", "Prefer pinned", "Prefer prediction", "Prefer poll",
         "Intelligent"},
        s.bannerStackMode,
        [](int val) {
            switch (val)
            {
                case 1: return QString("Prefer pinned");
                case 2: return QString("Prefer prediction");
                case 3: return QString("Intelligent");
                case 4: return QString("Prefer poll");
                default: return QString("Show all");
            }
        },
        [](DropdownArgs args) {
            if (args.value == "Prefer pinned") return 1;
            if (args.value == "Prefer prediction") return 2;
            if (args.value == "Prefer poll") return 4;
            if (args.value == "Intelligent") return 3;
            return 0;
        },
        false)
        ->setToolTip("How pinned, poll, and prediction banners share space "
                     "above chat when multiple are active.");

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Use message colors for tab alerts",
                            s.colorTabHighlightsByMessage)
        ->setTooltip("When a message highlights a tab, use the message's "
                     "highlight color for the tab indicator line.")
        ->addTo(layout);
    SettingWidget::checkbox("Hide mod actions on moderator usercards",
                            s.hideModActionsOnModUsercards)
        ->setTooltip(
            "Do not show timeout/ban buttons when clicking a moderator's "
            "name in chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show mod actions on mod usercards as lead mod",
                            s.showModActionsOnModUsercardsAsLeadMod)
        ->setTooltip("When you are lead moderator, still show mod action "
                     "buttons on other moderators' usercards.")
        ->addTo(layout);

    layout.addTitle("Nuke");
    SettingWidget::checkbox("Enable nuke preview",
                            s.nukePreviewEnabled)
        ->setTooltip("While typing /nuke, highlight matching messages in chat "
                     "as a preview.")
        ->addTo(layout);
    SettingWidget::checkbox("Show nuke summary", s.nukeShowSummary)
        ->setTooltip("Show a summary message in chat when a nuke finishes.")
        ->addTo(layout);
    SettingWidget::checkbox("Skip VIPs in nuke", s.nukeSkipVips)
        ->setTooltip("Do not target VIP users when running /nuke.")
        ->addTo(layout);
    SettingWidget::lineEdit("Nuke moderation reason",
                            s.nukeModerationMessage,
                            "Optional reason for timeout/ban")
        ->setTooltip("Reason sent with timeout or ban actions from /nuke.")
        ->addTo(layout);

    auto addNukeRangeDropdown =
        [&layout](const QString &label, auto &setting, const QString &tooltip,
                  const QStringList &options,
                  const std::vector<int> &values) {
            layout.addDropdown<int>(
                label, options, setting,
                [values, options](int val) {
                    for (int i = 0; i < (int)values.size(); ++i)
                    {
                        if (values[i] == val)
                            return options[i];
                    }
                    return options.last();
                },
                [values, options](DropdownArgs args) {
                    for (int i = 0; i < options.size(); ++i)
                    {
                        if (options[i] == args.value)
                            return values[i];
                    }
                    return values.back();
                },
                false)
                ->setToolTip(tooltip);
        };

    addNukeRangeDropdown(
        "Delete max range", s.nukeMaxDeleteRangeSeconds,
        "How far back /nuke delete can scan, and how long it watches for new "
        "messages.",
        {"1 min", "2 min", "5 min", "10 min", "30 min", "1 hour"},
        {60, 120, 300, 600, 1800, 3600});
    addNukeRangeDropdown(
        "Timeout max range", s.nukeMaxTimeoutRangeSeconds,
        "How far back /nuke timeout can scan, and how long it watches for new "
        "messages.",
        {"10 min", "30 min", "1 hour", "2 hours", "3 hours", "6 hours",
         "12 hours"},
        {600, 1800, 3600, 7200, 10800, 21600, 43200});
    addNukeRangeDropdown(
        "Ban max range", s.nukeMaxBanRangeSeconds,
        "How far back /nuke ban can scan, and how long it watches for new "
        "messages.",
        {"10 min", "30 min", "1 hour", "2 hours", "3 hours", "6 hours",
         "12 hours"},
        {600, 1800, 3600, 7200, 10800, 21600, 43200});

    layout.addTitle("Translation");
    SettingWidget::lineEdit("Default translation target language",
                            s.messageTranslationTargetLanguage, "e.g. en, fr, ja")
        ->setTooltip(
            "Language code used by /translate. Use /translateto <lang> "
            "<text> to override per message.")
        ->addTo(layout);

    layout.addTitle("Moderation Logging");
    SettingWidget::checkbox("Enable moderation logging (ModLogs)", s.enableModLogs)
        ->setTooltip("Log channel timeout and ban moderation events to the ModLogs folder.")
        ->addTo(layout);

    layout.addTitle("YouTube");
    layout.addDescription(
        "A single live chat poll can return several seconds' worth of "
        "messages at once; these control the delay used to stagger their "
        "display instead of showing them all at once.");
    SettingWidget::intInput("Minimum delay between messages",
                            s.youtubeMessageStaggerMinMs,
                            {.min = 0, .max = 5000, .suffix = " ms"})
        ->setTooltip("Shortest delay used between two staggered messages, "
                     "even if they were sent almost simultaneously.")
        ->addTo(layout);
    SettingWidget::intInput("Maximum delay between messages",
                            s.youtubeMessageStaggerMaxMs,
                            {.min = 0, .max = 5000, .suffix = " ms"})
        ->setTooltip("Longest delay used between two staggered messages, "
                     "even if they were actually sent far apart in time.")
        ->addTo(layout);

    layout.addTitle("Moderation");
    SettingWidget::checkbox("Show repeated-message counters",
                            s.enableRepeatedMessageDetector)
        ->setTooltip("Show repeated or very similar messages with an inline "
                     "counter such as x2, x3.")
        ->addTo(layout);
    SettingWidget::checkbox("Show only in moderation mode",
                            s.repeatedMessagesShowOnlyModerationMode)
        ->setTooltip("Only show repetition counters when inline mod buttons "
                     "are visible.")
        ->addTo(layout);
    SettingWidget::checkbox("Show counters in usercards",
                            s.repeatedMessagesShowInUsercards)
        ->setTooltip(
            "Show already-detected repeat counters in usercards when clicking "
            "a user.")
        ->addTo(layout);
    SettingWidget::checkbox("Only in channels where I can moderate",
                            s.repeatedMessagesOnlyModChannels)
        ->setTooltip("Only show repeat counters in channels where you can "
                     "moderate.")
        ->addTo(layout);
    SettingWidget::checkbox("Ignore VIPs", s.repeatedMessagesIgnoreVips)
        ->setTooltip("Do not mark repeated messages from VIPs.")
        ->addTo(layout);

    layout.addDropdown<int>(
        "Similarity sensitivity",
        {"Loose", "Soft", "Default", "Strict", "Exact only"},
        s.repeatedMessagesSensitivity,
        [](auto val) {
            switch (val)
            {
                case 0: return QString("Loose");
                case 1: return QString("Soft");
                case 3: return QString("Strict");
                case 4: return QString("Exact only");
                default: return QString("Default");
            }
        },
        [](auto args) {
            if (args.value == "Loose") return 0;
            if (args.value == "Soft") return 1;
            if (args.value == "Strict") return 3;
            if (args.value == "Exact only") return 4;
            return 2;
        },
        false)
        ->setToolTip(
            "How similar two messages need to be before they count as "
            "repeated.");

    SettingWidget::intInput("Repetition threshold",
                            s.repeatedMessagesRepetitionThreshold,
                            {.min = 2, .max = 20})
        ->setTooltip(
            "How many matching messages are required before the counter "
            "appears.")
        ->addTo(layout);

    SettingWidget::colorButton("Repeat counter color",
                               s.repeatedMessagesCounterColor)
        ->setTooltip("Text color for the inline repeated-message counter.")
        ->addTo(layout);

    layout.addStretch();

    // invisible element for width
    auto *inv = new BaseWidget(this);
    //    inv->setScaleIndependantWidth(600);
    layout.addWidget(inv);
}

void TechnorinoPage::initExtra()
{
    /// update cache path
    if (this->cachePath_)
    {
        getSettings()->cachePath.connect(
            [cachePath = this->cachePath_](const auto &, auto) mutable {
                QString newPath = getApp()->getPaths().cacheDirectory();

                QString pathShortened = "Current location: <a href=\"file:///" +
                                        newPath + "\">" +
                                        shortenString(newPath, 50) + "</a>";

                cachePath->setText(pathShortened);
                cachePath->setToolTip(newPath);
            });
    }
}

}  // namespace chatterino
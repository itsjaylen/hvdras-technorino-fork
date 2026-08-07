#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

class QLabel;
class QCheckBox;
class QComboBox;

namespace chatterino {

class GeneralPageView;
class DescriptionLabel;
struct DropdownArgs;
class Settings;

class TechnorinoPage : public SettingsPage
{
    Q_OBJECT

public:
    TechnorinoPage();

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);
    void initExtra();

    // Section helpers
    void initChatSection(GeneralPageView &layout, Settings &s);
    void initModerationLoggingSection(GeneralPageView &layout, Settings &s);
    void initYoutubeSection(GeneralPageView &layout, Settings &s);
    void initModerationSection(GeneralPageView &layout, Settings &s);

    QString getFont(const DropdownArgs &args) const;

    DescriptionLabel *cachePath_{};
    GeneralPageView *view_{};
};

}  // namespace chatterino
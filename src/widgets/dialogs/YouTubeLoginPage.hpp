// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

namespace chatterino {

class YouTubeLoginPage : public QWidget
{
public:
    YouTubeLoginPage();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct {
        QFormLayout *layout = nullptr;
        QLabel *topLabel = nullptr;
        QLineEdit *clientID = nullptr;
        QLineEdit *clientSecret = nullptr;
    } ui;
};

}  // namespace chatterino

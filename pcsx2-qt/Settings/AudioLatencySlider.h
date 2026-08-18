// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtWidgets/QSlider>

class AudioLatencySlider final : public QSlider
{
public:
	explicit AudioLatencySlider(QWidget* parent = nullptr);

	void setMinimumLatencyMarker(int latency_ms);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	int m_minimum_latency_ms = 0;
};

// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "AudioLatencySlider.h"

#include <QtGui/QPainter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionSlider>
#include <algorithm>

AudioLatencySlider::AudioLatencySlider(QWidget* parent)
	: QSlider(parent)
{
}

void AudioLatencySlider::setMinimumLatencyMarker(int latency_ms)
{
	if (m_minimum_latency_ms == latency_ms)
		return;

	m_minimum_latency_ms = latency_ms;
	update();
}

void AudioLatencySlider::paintEvent(QPaintEvent* event)
{
	QSlider::paintEvent(event);
	if (m_minimum_latency_ms < minimum() || m_minimum_latency_ms > maximum())
		return;

	QStyleOptionSlider option;
	initStyleOption(&option);
	option.sliderPosition = m_minimum_latency_ms;
	option.sliderValue = m_minimum_latency_ms;

	const QRect handle_rect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
	const QRect groove_rect = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
	const int marker_top = std::max(0, groove_rect.top() - 5);
	const int marker_bottom = std::min(height(), groove_rect.bottom() + 6);

	QPainter painter(this);
	painter.fillRect(handle_rect.center().x() - 1, marker_top, 3, marker_bottom - marker_top,
		palette().color(QPalette::Highlight));
}

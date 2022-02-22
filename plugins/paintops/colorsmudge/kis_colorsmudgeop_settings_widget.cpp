/*
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_colorsmudgeop_settings_widget.h"
#include "kis_brush_based_paintop_settings.h"
#include "kis_overlay_mode_option.h"
#include "kis_rate_option.h"
#include "kis_smudge_option_widget.h"
#include "kis_brush_option_widget.h"
#include "kis_smudge_radius_option.h"

#include <kis_properties_configuration.h>
#include <kis_paintop_settings_widget.h>
#include <kis_pressure_size_option.h>
#include <kis_pressure_ratio_option.h>
#include <kis_curve_option_widget.h>
#include <kis_pressure_paint_thickness_option.h>
#include <kis_pressure_paint_thickness_option_widget.h>
#include <kis_pressure_rotation_option.h>
#include <kis_pressure_scatter_option_widget.h>
#include <kis_pressure_opacity_option.h>
#include <kis_pressure_gradient_option.h>
#include <kis_airbrush_option_widget.h>
#include <kis_compositeop_option.h>
#include <kis_pressure_spacing_option_widget.h>
#include <kis_pressure_rate_option.h>
#include "kis_texture_option.h"
#include <kis_pressure_mirror_option_widget.h>
#include "kis_pressure_texture_strength_option.h"
#include "kis_pressure_hsv_option.h"
#include "kis_colorsmudgeop_settings.h"
#include "kis_signals_blocker.h"


KisColorSmudgeOpSettingsWidget::KisColorSmudgeOpSettingsWidget(QWidget* parent):
    KisBrushBasedPaintopOptionWidget(parent)
{
    setObjectName("brush option widget");
    setPrecisionEnabled(true);
    setHSLBrushTipEnabled(true);

    addPaintOpOption(new KisCompositeOpOption(true), i18n("Blending Mode"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureOpacityOption(), i18n("Transparent"), i18n("Opaque")), i18n("Opacity"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureSizeOption(), i18n("0%"), i18n("100%")), i18n("Size"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureRatioOption(), i18n("0%"), i18n("100%")), i18n("Ratio"));
    addPaintOpOption(new KisPressureSpacingOptionWidget(), i18n("Spacing"));
    addPaintOpOption(new KisPressureMirrorOptionWidget(), i18n("Mirror"));

    m_smudgeOptionWidget = new KisSmudgeOptionWidget();

    addPaintOpOption(m_smudgeOptionWidget, i18n("Smudge Length"));

    m_radiusStrengthOptionWidget = new KisCurveOptionWidget(new KisSmudgeRadiusOption(), i18n("0.0"), i18n("1.0"));
    addPaintOpOption(m_radiusStrengthOptionWidget, i18n("Smudge Radius"));

    addPaintOpOption(new KisCurveOptionWidget(new KisRateOption("ColorRate", KisPaintOpOption::GENERAL, false), i18n("0.0"), i18n("1.0")), i18nc("Color rate of active Foreground color", "Color Rate"));
    m_paintThicknessOptionWidget = new KisPressurePaintThicknessOptionWidget();
    addPaintOpOption(m_paintThicknessOptionWidget, i18n("Paint Thickness"));

    addPaintOpOption(new KisCurveOptionWidget(new KisPressureRotationOption(), i18n("-180°"), i18n("180°")), i18n("Rotation"));
    addPaintOpOption(new KisPressureScatterOptionWidget(), i18n("Scatter"));
    m_overlayOptionWidget = new KisOverlayModeOptionWidget();
    addPaintOpOption(m_overlayOptionWidget, i18n("Overlay Mode"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureGradientOption(), i18n("0%"), i18n("100%")), i18n("Gradient"));
    addPaintOpOption(new KisCurveOptionWidget(KisPressureHSVOption::createHueOption(), KisPressureHSVOption::hueMinLabel(), KisPressureHSVOption::huemaxLabel()), i18n("Hue"));
    addPaintOpOption(new KisCurveOptionWidget(KisPressureHSVOption::createSaturationOption(), KisPressureHSVOption::saturationMinLabel(), KisPressureHSVOption::saturationmaxLabel()), i18n("Saturation"));
    addPaintOpOption(new KisCurveOptionWidget(KisPressureHSVOption::createValueOption(), KisPressureHSVOption::valueMinLabel(), KisPressureHSVOption::valuemaxLabel()), i18nc("Label of Brightness value in Color Smudge brush engine options", "Value"));
    addPaintOpOption(new KisAirbrushOptionWidget(false), i18n("Airbrush"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureRateOption(), i18n("0%"), i18n("100%")), i18n("Rate"));

    addPaintOpOption(new KisTextureOption(), i18n("Pattern"));
    addPaintOpOption(new KisCurveOptionWidget(new KisPressureTextureStrengthOption(), i18n("Weak"), i18n("Strong")), i18n("Strength"));

    const KisBrushOptionWidget* brushOption = brushOptionWidget();
    connect(brushOption, SIGNAL(sigSettingChanged()), SLOT(slotBrushOptionChanged()));
}

KisColorSmudgeOpSettingsWidget::~KisColorSmudgeOpSettingsWidget() { }

KisPropertiesConfigurationSP KisColorSmudgeOpSettingsWidget::configuration() const
{
    KisColorSmudgeOpSettingsSP config = new KisColorSmudgeOpSettings(resourcesInterface());
    config->setProperty("paintop", "colorsmudge");
    writeConfiguration(config);
    return config;
}

void KisColorSmudgeOpSettingsWidget::notifyPageChanged()
{
    KisBrushSP brush = this->brush();
    bool pierced =  brush ? brush->isPiercedApprox() : false;
    m_smudgeOptionWidget->updateBrushPierced(pierced);

    //If brush is a mask, it can use either engine, but if its not, it must use the new engine
    if (brush) {
        m_smudgeOptionWidget->setUseNewEngineCheckboxEnabled(brush->brushApplication() == ALPHAMASK);
        m_paintThicknessOptionWidget->setEnabled(brush->preserveLightness());
        m_overlayOptionWidget->setEnabled(brush->brushApplication() != LIGHTNESSMAP);
        m_radiusStrengthOptionWidget->updateRange(0.0, m_smudgeOptionWidget->useNewEngine() ? 1.0 : 3.0);
    }
}

void KisColorSmudgeOpSettingsWidget::slotBrushOptionChanged() {
    notifyPageChanged();
}

void KisColorSmudgeOpSettingsWidget::fixNewEngineOption() const
{
    KisBrushSP brush = const_cast<KisColorSmudgeOpSettingsWidget*>(this)->brush();

    if (brush) {
        if (brush->brushApplication() != ALPHAMASK) {
            KisSignalsBlocker b(m_smudgeOptionWidget);
            m_smudgeOptionWidget->setUseNewEngine(true);
        }
    }
}

void KisColorSmudgeOpSettingsWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    KisBrushBasedPaintopOptionWidget::setConfiguration(config);
    fixNewEngineOption();
    // make sure the options sanity (including the smudge radius range) is updated after brush reset
    notifyPageChanged();
}

void KisColorSmudgeOpSettingsWidget::writeConfiguration(KisPropertiesConfigurationSP config) const
{
    fixNewEngineOption();
    KisBrushBasedPaintopOptionWidget::writeConfiguration(config);
}

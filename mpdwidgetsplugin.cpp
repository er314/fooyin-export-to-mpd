#include "mpdwidgetsplugin.h"
#include "mpdcontrols.h"

#include <gui/widgetprovider.h>

MpdWidgetsPlugin::MpdWidgetsPlugin()
    : m_controls{new MpdControls(this)}
{}

void MpdWidgetsPlugin::initialise(const Fooyin::CorePluginContext& context)
{
    m_controls->setFooyinServices(context.library);
}

void MpdWidgetsPlugin::initialise(const Fooyin::GuiPluginContext& context)
{
    m_controls->setGuiServices(context.playlistSelection,
                               context.trackSelection);

    context.widgetProvider->registerWidget(
        "MPDControls",
        [this]() { return new MpdControlsWidget(m_controls); },
        "MPD controls");
}

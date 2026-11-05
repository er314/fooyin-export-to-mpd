#pragma once

#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/guiplugin.h>

class MpdControls;

class MpdWidgetsPlugin final : public QObject,
                               public Fooyin::Plugin,
                               public Fooyin::CorePlugin,
                               public Fooyin::GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "metadata.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)

public:
    MpdWidgetsPlugin();

    void initialise(const Fooyin::CorePluginContext& context) override;
    void initialise(const Fooyin::GuiPluginContext& context) override;

private:
    MpdControls* m_controls;
};

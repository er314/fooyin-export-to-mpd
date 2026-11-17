
#pragma once

#include <core/track.h>
#include <gui/fywidget.h>

#include <QObject>
#include <QString>
#include <QList>

class QCheckBox;
class QPushButton;
class QToolButton;
class QPoint;

struct MpdAudioOutput
{
    unsigned id;
    QString name;
    QString plugin;
    bool enabled;
};

namespace Fooyin {

class CurrentPlaylistController;
class MusicLibrary;
class TrackSelectionController;

}

class MpdControls final : public QObject
{
    Q_OBJECT

public:
    explicit MpdControls(QObject* parent = nullptr);

    void setFooyinServices(Fooyin::MusicLibrary* library);

    void setGuiServices(Fooyin::CurrentPlaylistController* playlistSelection,
                        Fooyin::TrackSelectionController* trackSelection);

    bool tracksForCurrentPlaylist(Fooyin::TrackList* tracks,
                                  QString* error) const;

    void addTracks(const Fooyin::TrackList& tracks,
                   bool toStoredPlaylist,
                   bool clearFirst);

    // MPD playback controls.
    void previous();
    void togglePlayback();
    void next();
    void stopPlayback();

    // MPD audio outputs.
    QList<MpdAudioOutput> availableOutputs(QString* error = nullptr);
    void selectOutput(const MpdAudioOutput& output);

    [[nodiscard]] int selectedOutputId() const
    {
        return m_selectedOutputId;
    }

signals:
    void busyChanged(bool busy);
    void statusChanged(const QString& message);

    // true = Pause icon/mode; false = Play icon/mode.
    void playbackModeChanged(bool pauseMode);

private:
    Fooyin::MusicLibrary* m_library{nullptr};

    Fooyin::CurrentPlaylistController* m_playlistSelection{nullptr};
    Fooyin::TrackSelectionController* m_trackSelection{nullptr};

    bool m_busy{false};

    int m_selectedOutputId{-1};
    QString m_selectedOutputName;
    QString m_selectedOutputPlugin;
    bool m_pauseMode{false};
};

class MpdControlsWidget final : public Fooyin::FyWidget
{
    Q_OBJECT

public:
    explicit MpdControlsWidget(MpdControls* controls,
                               QWidget* parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString layoutName() const override;

private:
    void add(bool toStoredPlaylist);
    void showResult(const QString& message);
    void showOutputMenu(const QPoint& pos);

    MpdControls* m_controls;

    QPushButton* m_queueButton;
    QPushButton* m_playlistButton;
    QCheckBox* m_clearCheckBox;

    QToolButton* m_previousButton;
    QToolButton* m_playPauseButton;
    QToolButton* m_nextButton;
};
